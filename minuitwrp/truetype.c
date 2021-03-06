#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>

#include "minui.h"

#include <cutils/hashmap.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include <pixelflinger/pixelflinger.h>
#include <pthread.h>

#define STRING_CACHE_MAX_ENTRIES 400
#define STRING_CACHE_TRUNCATE_ENTRIES 150

typedef struct
{
    int size;
    int dpi;
    char *path;
} TrueTypeFontKey;

typedef struct
{
    int type;
    int refcount;
    int size;
    int dpi;
    int max_height;
    int base;
    FT_Face face;
    Hashmap *glyph_cache;
    Hashmap *string_cache;
    struct StringCacheEntry *string_cache_head;
    struct StringCacheEntry *string_cache_tail;
    pthread_mutex_t mutex;
    TrueTypeFontKey *key;
} TrueTypeFont;

typedef struct
{
    FT_BBox bbox;
    FT_BitmapGlyph glyph;
} TrueTypeCacheEntry;

typedef struct
{
    char *text;
    int max_width;
} StringCacheKey;

struct StringCacheEntry
{
    GGLSurface surface;
    int rendered_len;
    StringCacheKey *key;
    struct StringCacheEntry *prev;
    struct StringCacheEntry *next;
};

typedef struct StringCacheEntry StringCacheEntry;

typedef struct 
{
    FT_Library ft_library;
    Hashmap *fonts;
    pthread_mutex_t mutex;
} FontData;

static FontData font_data = {
    .ft_library = NULL,
    .fonts = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))

// 32bit FNV-1a hash algorithm
// http://isthe.com/chongo/tech/comp/fnv/#FNV-1a
static const uint32_t FNV_prime = 16777619U;
static const uint32_t offset_basis = 2166136261U;

int utf8_to_unicode(unsigned c1, unsigned c2, unsigned c3)
{
    unsigned short unicode;

    unicode = (c1 & 0x1F) << 12;
    unicode |= (c2 & 0x3F) << 6;
    unicode |= (c3 & 0x3F);

    return unicode;
}

static uint32_t fnv_hash(void *data, uint32_t len)
{
    uint8_t *d8 = data;
    uint32_t *d32 = data;
    uint32_t i, max;
    uint32_t hash = offset_basis;

    max = len/4;

    // 32 bit data
    for(i = 0; i < max; ++i)
    {
        hash ^= *d32++;
        hash *= FNV_prime;
    }

    // last bits
    for(i *= 4; i < len; ++i)
    {
        hash ^= (uint32_t) d8[i];
        hash *= FNV_prime;
    }
    return hash;
}

static inline uint32_t fnv_hash_add(uint32_t cur_hash, uint32_t word)
{
    cur_hash ^= word;
    cur_hash *= FNV_prime;
    return cur_hash;
}

static bool gr_ttf_string_cache_equals(void *keyA, void *keyB)
{
    StringCacheKey *a = keyA;
    StringCacheKey *b = keyB;
    return a->max_width == b->max_width && strcmp(a->text, b->text) == 0;
}

static int gr_ttf_string_cache_hash(void *key)
{
    StringCacheKey *k = key;
    return fnv_hash(k->text, strlen(k->text));
}

static bool gr_ttf_font_cache_equals(void *keyA, void *keyB)
{
    TrueTypeFontKey *a = keyA;
    TrueTypeFontKey *b = keyB;
    return (a->size == b->size) && (a->dpi == b->dpi) && !strcmp(a->path, b->path);
}

static int gr_ttf_font_cache_hash(void *key)
{
    TrueTypeFontKey *k = key;

    uint32_t hash = fnv_hash(k->path, strlen(k->path));
    hash = fnv_hash_add(hash, k->size);
    hash = fnv_hash_add(hash, k->dpi);
    return hash;
}

void *gr_ttf_loadFont(const char *filename, int size, int dpi)
{
    int error;
    TrueTypeFont *res = NULL;

    pthread_mutex_lock(&font_data.mutex);

    if(font_data.fonts)
    {
        TrueTypeFontKey k = {
            .size = size,
            .dpi = dpi,
            .path = (char*)filename
        };

        res = hashmapGet(font_data.fonts, &k);
        if(res)
        {
            ++res->refcount;
            goto exit;
        }
    }

    if(!font_data.ft_library)
    {
        error = FT_Init_FreeType(&font_data.ft_library);
        if(error)
        {
            fprintf(stderr, "Failed to init libfreetype! %d\n", error);
            goto exit;
        }
    }

    FT_Face face;
    error = FT_New_Face(font_data.ft_library, filename, 0, &face);
    if(error)
    {
        fprintf(stderr, "Failed to load truetype face %s: %d\n", filename, error);
        goto exit;
    }

    error = FT_Set_Char_Size(face, 0, size*16, dpi, dpi);
    if(error)
    {
         fprintf(stderr, "Failed to set truetype face size to %d, dpi %d: %d\n", size, dpi, error);
         FT_Done_Face(face);
         goto exit;
    }

    res = malloc(sizeof(TrueTypeFont));
    memset(res, 0, sizeof(TrueTypeFont));
    res->type = FONT_TYPE_TTF;
    res->size = size;
    res->dpi = dpi;
    res->face = face;
    res->max_height = -1;
    res->base = -1;
    res->refcount = 1;
    res->glyph_cache = hashmapCreate(32, hashmapIntHash, hashmapIntEquals);
    res->string_cache = hashmapCreate(128, gr_ttf_string_cache_hash, gr_ttf_string_cache_equals);
    pthread_mutex_init(&res->mutex, 0);

    if(!font_data.fonts)
        font_data.fonts = hashmapCreate(4, gr_ttf_font_cache_hash, gr_ttf_font_cache_equals);

    TrueTypeFontKey *key = malloc(sizeof(TrueTypeFontKey));
    memset(key, 0, sizeof(TrueTypeFontKey));
    key->path = strdup(filename);
    key->size = size;
    key->dpi = dpi;

    res->key = key;

    hashmapPut(font_data.fonts, key, res);

exit:
    pthread_mutex_unlock(&font_data.mutex);
    return res;
}

static bool gr_ttf_freeFontCache(void *key, void *value, void *context)
{
    TrueTypeCacheEntry *e = value;
    FT_Done_Glyph((FT_Glyph)e->glyph);
    free(e);
    free(key);
    return true;
}

static bool gr_ttf_freeStringCache(void *key, void *value, void *context)
{
    StringCacheKey *k = key;
    free(k->text);
    free(k);

    StringCacheEntry *e = value;
    free(e->surface.data);
    free(e);
    return true;
}

void gr_ttf_freeFont(void *font)
{
    pthread_mutex_lock(&font_data.mutex);

    TrueTypeFont *d = font;

    if(--d->refcount == 0)
    {
        hashmapRemove(font_data.fonts, d->key);

        if(hashmapSize(font_data.fonts) == 0)
        {
            hashmapFree(font_data.fonts);
            font_data.fonts = NULL;
        }

        free(d->key->path);
        free(d->key);

        FT_Done_Face(d->face);
        hashmapForEach(d->string_cache, gr_ttf_freeStringCache, NULL);
        hashmapFree(d->string_cache);
        hashmapForEach(d->glyph_cache, gr_ttf_freeFontCache, NULL);
        hashmapFree(d->glyph_cache);
        pthread_mutex_destroy(&d->mutex);
        free(d);
    }

    pthread_mutex_unlock(&font_data.mutex);
}

static TrueTypeCacheEntry *gr_ttf_glyph_cache_peek(TrueTypeFont *font, int char_index)
{
    return hashmapGet(font->glyph_cache, &char_index);
}

static TrueTypeCacheEntry *gr_ttf_glyph_cache_get(TrueTypeFont *font, int char_index)
{
    TrueTypeCacheEntry *res = hashmapGet(font->glyph_cache, &char_index);
    if(!res)
    {
        int error = FT_Load_Glyph(font->face, char_index, FT_LOAD_RENDER);
        if(error)
        {
            fprintf(stderr, "Failed to load glyph idx %d: %d\n", char_index, error);
            return NULL;
        }

        FT_BitmapGlyph glyph;
        error = FT_Get_Glyph(font->face->glyph, (FT_Glyph*)&glyph);
        if(error)
        {
            fprintf(stderr, "Failed to copy glyph %d: %d\n", char_index, error);
            return NULL;
        }

        res = malloc(sizeof(TrueTypeCacheEntry));
        memset(res, 0, sizeof(TrueTypeCacheEntry));
        res->glyph = glyph;
        FT_Glyph_Get_CBox((FT_Glyph)glyph, FT_GLYPH_BBOX_PIXELS, &res->bbox);

        int *key = malloc(sizeof(int));
        *key = char_index;

        hashmapPut(font->glyph_cache, key, res);
    }

    return res;
}

static int gr_ttf_copy_glyph_to_surface(GGLSurface *dest, FT_BitmapGlyph glyph, int offX, int offY, int base)
{
    int y;
    uint8_t *src_itr = glyph->bitmap.buffer;
    uint8_t *dest_itr = dest->data;

    if(glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
    {
        fprintf(stderr, "Unsupported pixel mode in FT_BitmapGlyph %d\n", glyph->bitmap.pixel_mode);
        return -1;
    }

    dest_itr += (offY + base - glyph->top)*dest->stride + (offX + glyph->left);

    for(y = 0; y < glyph->bitmap.rows; ++y)
    {
        memcpy(dest_itr, src_itr, glyph->bitmap.width);
        src_itr += glyph->bitmap.pitch;
        dest_itr += dest->stride;
    }
    return 0;
}

static int gr_ttf_render_text(TrueTypeFont *font, GGLSurface *surface, const char *text, int max_width)
{
    TrueTypeFont *f = font;
    TrueTypeCacheEntry *ent;
    int max_len = 0, total_w = 0;
    unsigned char c, c2, c3;
    int i, x, diff, char_idx, prev_idx = 0;
    int height, base;
    FT_Vector delta;
    uint8_t *data = NULL;
    const char *text_itr = text;
    int *char_idxs;
	
    char_idxs = (int*)malloc(strlen(text) * sizeof(int));
    while((c = *text_itr++))
    {
        if (c < 0x80)
        {
            char_idx = FT_Get_Char_Index(f->face, c);
        }
        else
        {
            c2 = *text_itr++;
            c3 = *text_itr++;
            char_idx = FT_Get_Char_Index(f->face, utf8_to_unicode(c, c2, c3));
        }
        char_idxs[max_len] = char_idx;
        ent = gr_ttf_glyph_cache_get(f, char_idx);
        if(ent)
        {
            diff = ent->glyph->root.advance.x >> 16;

            if(FT_HAS_KERNING(f->face) && prev_idx && char_idx)
            {
                FT_Get_Kerning(f->face, prev_idx, char_idx, FT_KERNING_DEFAULT, &delta);
                diff += delta.x >> 6;
            }

            if(max_width != -1 && total_w + diff > max_width)
                break;

            total_w += diff;
        }
        prev_idx = char_idx;
        ++max_len;
    }

    if(font->max_height == -1)
        gr_ttf_getMaxFontHeight(font);

    if(font->max_height == -1) 
    {
        free(char_idxs);
    return -1;
    }

    height = font->max_height;

    data = malloc(total_w*height);
    memset(data, 0, total_w*height);
    x = 0;
    prev_idx = 0;

    surface->version = sizeof(*surface);
    surface->width = total_w;
    surface->height = height;
    surface->stride = total_w;
    surface->data = (void*)data;
    surface->format = GGL_PIXEL_FORMAT_A_8;

    for(i = 0; i < max_len; ++i)
    {
        char_idx = char_idxs[i];
        if(FT_HAS_KERNING(f->face) && prev_idx && char_idx)
        {
            FT_Get_Kerning(f->face, prev_idx, char_idx, FT_KERNING_DEFAULT, &delta);
            x += delta.x >> 6;
        }

        ent = gr_ttf_glyph_cache_get(f, char_idx);
        if(ent)
        {
            gr_ttf_copy_glyph_to_surface(surface, ent->glyph, x, 0, font->base);
            x += ent->glyph->root.advance.x >> 16;
        }

        prev_idx = char_idx;
    }

    free(char_idxs);
    return max_len;
}

static StringCacheEntry *gr_ttf_string_cache_peek(TrueTypeFont *font, const char *text, int max_width)
{
    StringCacheEntry *res;
    StringCacheKey k = {
        .text = (char*)text,
        .max_width = max_width
    };

    return hashmapGet(font->string_cache, &k);
}

static StringCacheEntry *gr_ttf_string_cache_get(TrueTypeFont *font, const char *text, int max_width)
{
    StringCacheEntry *res;
    StringCacheKey k = {
        .text = (char*)text,
        .max_width = max_width
    };

    res = hashmapGet(font->string_cache, &k);
    if(!res)
    {
        res = malloc(sizeof(StringCacheEntry));
        memset(res, 0, sizeof(StringCacheEntry));
        res->rendered_len = gr_ttf_render_text(font, &res->surface, text, max_width);
        if(res->rendered_len < 0)
        {
            free(res);
            return NULL;
        }

        StringCacheKey *new_key = malloc(sizeof(StringCacheKey));
        memset(new_key, 0, sizeof(StringCacheKey));
        new_key->max_width = max_width;
        new_key->text = strdup(text);

        res->key = new_key;

        if(font->string_cache_tail)
        {
            res->prev = font->string_cache_tail;
            res->prev->next = res;
        }
        else
            font->string_cache_head = res;
        font->string_cache_tail = res;

        hashmapPut(font->string_cache, new_key, res);
    }
    else if(res->next)
    {
        // move this entry to the tail of the linked list
        // if it isn't already there
        if(res->prev)
            res->prev->next = res->next;

        res->next->prev = res->prev;

        if(!res->prev)
            font->string_cache_head = res->next;

        res->next = NULL;
        res->prev = font->string_cache_tail;
        res->prev->next = res;
        font->string_cache_tail = res;

        // truncate old entries
        if(hashmapSize(font->string_cache) >= STRING_CACHE_MAX_ENTRIES)
        {
            printf("Truncating string cache entries.\n");
            int i;
            StringCacheEntry *ent;
            for(i = 0; i < STRING_CACHE_TRUNCATE_ENTRIES; ++i)
            {
                ent = font->string_cache_head;
                font->string_cache_head = ent->next;
                font->string_cache_head->prev = NULL;

                hashmapRemove(font->string_cache, ent->key);

                gr_ttf_freeStringCache(ent->key, ent, NULL);
            }
        }
    }
    return res;
}

int gr_ttf_measureEx(const char *s, void *font)
{
    TrueTypeFont *f = font;
    int res = -1;

    pthread_mutex_lock(&f->mutex);
    StringCacheEntry *e = gr_ttf_string_cache_get(font, s, -1);
    if(e)
        res = e->surface.width;
    pthread_mutex_unlock(&f->mutex);

    return res;
}

int gr_ttf_maxExW(const char *s, void *font, int max_width)
{
    TrueTypeFont *f = font;
    TrueTypeCacheEntry *ent;
    int max_len = 0, total_w = 0;
    unsigned char c, c2, c3;
    int char_idx, prev_idx = 0;
    FT_Vector delta;
    StringCacheEntry *e;

    pthread_mutex_lock(&f->mutex);

    e = gr_ttf_string_cache_peek(font, s, max_width);
    if(e)
    {
        max_len = e->rendered_len;
        pthread_mutex_unlock(&f->mutex);
        return max_len;
    }

    for(; (c = *s++); ++max_len)
    {
        if (c < 0x80)
        {
            char_idx = FT_Get_Char_Index(f->face, c);
        }
        else
        {
            c2 = *s++;
            c3 = *s++;
            char_idx = FT_Get_Char_Index(f->face, utf8_to_unicode(c, c2, c3));
        }
        if(FT_HAS_KERNING(f->face) && prev_idx && char_idx)
        {
            FT_Get_Kerning(f->face, prev_idx, char_idx, FT_KERNING_DEFAULT, &delta);
            total_w += delta.x >> 6;
        }
        prev_idx = char_idx;

        if(total_w > max_width)
            break;

        ent = gr_ttf_glyph_cache_get(f, char_idx);
        if(!ent)
            continue;

        total_w += ent->glyph->root.advance.x >> 16;
    }
    pthread_mutex_unlock(&f->mutex);
    return max_len > 0 ? max_len - 1 : 0;
}

int gr_ttf_textExWH(void *context, int x, int y, const char *s, void *pFont, int max_width, int max_height)
{
    GGLContext *gl = context;
    TrueTypeFont *font = pFont;

    // not actualy max width, but max_width + x
    if(max_width != -1)
    {
        max_width -= x;
        if(max_width <= 0)
            return 0;
    }

    pthread_mutex_lock(&font->mutex);

    StringCacheEntry *e = gr_ttf_string_cache_get(font, s, max_width);
    if(!e)
    {
        pthread_mutex_unlock(&font->mutex);
        return -1;
    }

    int y_bottom = y + e->surface.height;
    int res = e->rendered_len;

    if(max_height != -1 && max_height < y_bottom)
    {
        y_bottom = max_height;
        if(y_bottom <= y)
        {
            pthread_mutex_unlock(&font->mutex);
            return 0;
        }
    }

    gl->bindTexture(gl, &e->surface);
    gl->texEnvi(gl, GGL_TEXTURE_ENV, GGL_TEXTURE_ENV_MODE, GGL_REPLACE);
    gl->texGeni(gl, GGL_S, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->texGeni(gl, GGL_T, GGL_TEXTURE_GEN_MODE, GGL_ONE_TO_ONE);
    gl->enable(gl, GGL_TEXTURE_2D);

    gl->texCoord2i(gl, -x, -y);
    gl->recti(gl, x, y, x + e->surface.width, y_bottom);

    pthread_mutex_unlock(&font->mutex);
    return res;
}

int gr_ttf_getMaxFontHeight(void *font)
{
    int res;
    TrueTypeFont *f = font;

    pthread_mutex_lock(&f->mutex);

    if(f->max_height == -1)
    {
        char c;
        int char_idx;
        int error;
        FT_Glyph glyph;
        FT_BBox bbox;
        FT_BBox bbox_glyph;
        TrueTypeCacheEntry *ent;

        bbox.yMin = bbox_glyph.yMin = LONG_MAX;
        bbox.yMax = bbox_glyph.yMax = LONG_MIN;

        for(c = '!'; c <= '~'; ++c)
        {
            char_idx = FT_Get_Char_Index(f->face, c);
            ent = gr_ttf_glyph_cache_peek(f, char_idx);
            if(ent)
            {
                bbox.yMin = MIN(bbox.yMin, ent->bbox.yMin);
                bbox.yMax = MAX(bbox.yMax, ent->bbox.yMax);
            }
            else
            {
                error = FT_Load_Glyph(f->face, char_idx, 0);
                if(error)
                    continue;

                error = FT_Get_Glyph(f->face->glyph, &glyph);
                if(error)
                    continue;

                FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &bbox_glyph);
                bbox.yMin = MIN(bbox.yMin, bbox_glyph.yMin);
                bbox.yMax = MAX(bbox.yMax, bbox_glyph.yMax);

                FT_Done_Glyph(glyph);
            }
        }

        if(bbox.yMin > bbox.yMax)
            bbox.yMin = bbox.yMax = 0;

        f->max_height = bbox.yMax - bbox.yMin;
        f->base = bbox.yMax;

        // FIXME: twrp fonts have some padding on top, I'll add it here
        // Should be fixed in the themes
        f->max_height += f->size / 4;
        f->base += f->size / 4;
    }

    res = f->max_height;

    pthread_mutex_unlock(&f->mutex);
    return res;
}

static bool gr_ttf_dump_stats_count_string_cache(void *key, void *value, void *context)
{
    int *string_cache_size = context;
    StringCacheEntry *e = value;
    *string_cache_size += e->surface.height*e->surface.width + sizeof(StringCacheEntry);
    return true;
}

static bool gr_ttf_dump_stats_font(void *key, void *value, void *context)
{
    TrueTypeFontKey *k = key;
    TrueTypeFont *f = value;
    int *total_string_cache_size = context;
    int string_cache_size = 0;

    pthread_mutex_lock(&f->mutex);

    hashmapForEach(f->string_cache, gr_ttf_dump_stats_count_string_cache, &string_cache_size);

    printf("  Font %s (size %d, dpi %d):\n"
            "    refcount: %d\n"
            "    max_height: %d\n"
            "    base: %d\n"
            "    glyph_cache: %d entries\n"
            "    string_cache: %d entries (%.2f kB)\n",
            k->path, k->size, k->dpi,
            f->refcount, f->max_height, f->base,
            hashmapSize(f->glyph_cache),
            hashmapSize(f->string_cache), ((double)string_cache_size)/1024);

    pthread_mutex_unlock(&f->mutex);

    *total_string_cache_size += string_cache_size;
    return true;
}

void gr_ttf_dump_stats(void)
{
    pthread_mutex_lock(&font_data.mutex);

    printf("TrueType fonts system stats: ");
    if(!font_data.fonts)
        printf("no truetype fonts loaded.\n");
    else
    {
        int total_string_cache_size = 0;
        printf("%d fonts loaded.\n", hashmapSize(font_data.fonts));
        hashmapForEach(font_data.fonts, gr_ttf_dump_stats_font, &total_string_cache_size);
        printf("  Total string cache size: %.2f kB\n", ((double)total_string_cache_size)/1024);
    }

    pthread_mutex_unlock(&font_data.mutex);
}
