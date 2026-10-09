/*
 * SwanStationPS5 - TrueType text (Inter) on top of the platform mesh API.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Glyphs for Latin-1 are packed into one atlas per (weight, size bucket) the
 * first time that size is used; a line of text is then a single mesh draw.
 * Japanese: the characters the current language's strings use are packed into
 * the same atlases from a Noto Sans JP subset (see i18n.c).
 */
#include "text.h"

#include "../i18n.h"

#include "../platform/platform.h"
#include "stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_CP 32
#define CP_COUNT 224 /* U+0020..U+00FF */
#define MAX_ATLASES 16
#define MAX_EXTRA 1024

typedef struct
{
    int weight, px;
    int width, height;
    float ascent;
    stbtt_packedchar chars[CP_COUNT];
    stbtt_packedchar *extra; /* parallel to extra_cp[] */
    PlatTexture *texture;
} Atlas;

static unsigned char *font_data[3]; /* regular, bold (the theme's), mono */
static stbtt_fontinfo font_info[3];
static bool font_ok[3];
static Atlas atlases[MAX_ATLASES];
static int atlas_count;

/* Japanese fallback font and the characters packed from it, sorted. */
static unsigned char *jp_data;
static stbtt_fontinfo jp_info;
static bool jp_ok;
static int extra_cp[MAX_EXTRA];
static int extra_count;
static bool extra_dirty = true;

static const int buckets[] = {16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 128};

static unsigned char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static void free_atlases(void);

/* The interface's two weights (the theme's); Inter when a file is missing. */
static bool load_fonts(const char *regular, const char *bold)
{
    const char *files[2] = {regular, bold};
    for (int w = 0; w < 2; ++w)
    {
        char path[SwanStationPS5_PATH_MAX];
        plat_asset_path(path, sizeof(path), files[w]);
        unsigned char *data = read_file(path);
        stbtt_fontinfo info;
        bool ok = data && stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0));
        if (!ok)
        {
            SwanStationPS5_log("font missing or invalid: %s", path);
            free(data);
            if (font_ok[w])
                continue; /* keep the current one */
            plat_asset_path(path, sizeof(path), w ? "fonts/Inter-600.ttf" : "fonts/Inter-400.ttf");
            data = read_file(path);
            ok = data && stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0));
            if (!ok)
            {
                free(data);
                continue;
            }
        }
        free(font_data[w]);
        font_data[w] = data;
        font_info[w] = info;
        font_ok[w] = true;
    }
    if (!font_ok[FONT_BOLD] && font_ok[FONT_REGULAR])
    {
        font_info[FONT_BOLD] = font_info[FONT_REGULAR];
        font_ok[FONT_BOLD] = true;
    }
    return font_ok[FONT_REGULAR];
}

void text_set_fonts(const char *regular, const char *bold)
{
    load_fonts(regular, bold);
    free_atlases(); /* glyphs are packed again with the new faces */
    extra_dirty = true;
}

bool text_init(void)
{
    load_fonts("fonts/Inter-400.ttf", "fonts/Inter-600.ttf");
    char mono_path[SwanStationPS5_PATH_MAX];
    plat_asset_path(mono_path, sizeof(mono_path), "fonts/IBMPlexMono-400.ttf");
    font_data[FONT_MONO] = read_file(mono_path);
    font_ok[FONT_MONO] = font_data[FONT_MONO] &&
                         stbtt_InitFont(&font_info[FONT_MONO], font_data[FONT_MONO],
                                        stbtt_GetFontOffsetForIndex(font_data[FONT_MONO], 0));
    if (!font_ok[FONT_MONO])
    {
        SwanStationPS5_log("font missing or invalid: %s (guides use the regular face)", mono_path);
        free(font_data[FONT_MONO]);
        font_data[FONT_MONO] = NULL;
    }
    char jp_path[SwanStationPS5_PATH_MAX];
    plat_asset_path(jp_path, sizeof(jp_path), "fonts/NotoSansJP-SwanStationPS5.ttf");
    jp_data = read_file(jp_path);
    jp_ok = jp_data && stbtt_InitFont(&jp_info, jp_data, stbtt_GetFontOffsetForIndex(jp_data, 0));
    if (!jp_ok)
        SwanStationPS5_log("font missing or invalid: %s (Japanese will show as ?)", jp_path);
    return font_ok[FONT_REGULAR];
}

static void free_atlases(void)
{
    for (int i = 0; i < atlas_count; ++i)
    {
        plat_texture_free(atlases[i].texture);
        free(atlases[i].extra);
        atlases[i].extra = NULL;
    }
    atlas_count = 0;
}

void text_shutdown(void)
{
    free_atlases();
    for (int w = 0; w < 3; ++w)
        free(font_data[w]);
    free(jp_data);
}

static int decode(const char **p);

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* The non-Latin-1 characters the current language needs (always including the
 * language selector's own names). */
static void collect_extra(void)
{
    extra_dirty = false;
    extra_count = 0;
    if (!jp_ok)
        return;
    int lang = i18n_get();
    for (int i = 0;; ++i)
    {
        if (i >= LANG_COUNT && lang != LANG_JA)
            break;
        const char *s = i18n_string(lang, i);
        if (!s)
            break;
        while (*s)
        {
            int cp = decode(&s);
            if (cp <= 0xff || extra_count == MAX_EXTRA || !stbtt_FindGlyphIndex(&jp_info, cp))
                continue;
            bool seen = false;
            for (int k = 0; k < extra_count && !seen; ++k)
                seen = extra_cp[k] == cp;
            if (!seen)
                extra_cp[extra_count++] = cp;
        }
    }
    qsort(extra_cp, (size_t)extra_count, sizeof(int), cmp_int);
}

void text_language_changed(void)
{
    free_atlases();
    extra_dirty = true;
}

/* Japanese glyphs are drawn at Inter's em size for the same pixel height. */
static float jp_em_px(const stbtt_fontinfo *main, float px)
{
    return stbtt_ScaleForPixelHeight(main, px) / stbtt_ScaleForMappingEmToPixels(main, 1.0f);
}

static int bucket_for(float size)
{
    for (size_t i = 0; i < sizeof(buckets) / sizeof(buckets[0]); ++i)
        if (buckets[i] >= size - 0.5f)
            return buckets[i];
    return buckets[sizeof(buckets) / sizeof(buckets[0]) - 1];
}

static Atlas *get_atlas(int weight, float size)
{
    weight = weight == FONT_MONO && font_ok[FONT_MONO] ? FONT_MONO : weight ? FONT_BOLD : FONT_REGULAR;
    if (!font_ok[weight])
        return NULL;
    int px = bucket_for(size);
    for (int i = 0; i < atlas_count; ++i)
        if (atlases[i].weight == weight && atlases[i].px == px)
            return &atlases[i];
    if (atlas_count == MAX_ATLASES)
        return &atlases[0];
    if (extra_dirty)
        collect_extra();

    Atlas *a = &atlases[atlas_count];
    int over = px <= 32 ? 2 : 1;
    a->width = px <= 32 ? 512 : px <= 64 ? 1024 : 2048;
    a->height = px <= 48 ? 512 : 1024;
    if (extra_count)
    {
        /* room for the Japanese glyphs too: about (1.25 px)^2 each */
        double need = (CP_COUNT * 0.6 + extra_count) * (px * 1.25) * (px * 1.25) * over * 1.3;
        while ((double)a->width * a->height < need && a->height < 4096)
        {
            if (a->height < a->width)
                a->height *= 2;
            else
                a->width *= 2;
        }
        a->extra = calloc((size_t)extra_count, sizeof(stbtt_packedchar));
    }
    unsigned char *alpha = calloc((size_t)a->width * a->height, 1);
    if (!alpha)
        return NULL;
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, alpha, a->width, a->height, 0, 1, NULL);
    stbtt_PackSetOversampling(&pc, (unsigned)over, 1);
    stbtt_PackFontRange(&pc, font_data[weight] ? font_data[weight] : font_data[0], 0,
                        (float)px, FIRST_CP, CP_COUNT, a->chars);
    if (a->extra)
    {
        stbtt_pack_range r = {0};
        r.font_size = STBTT_POINT_SIZE(jp_em_px(&font_info[weight], (float)px));
        r.array_of_unicode_codepoints = extra_cp;
        r.num_chars = extra_count;
        r.chardata_for_range = a->extra;
        stbtt_PackFontRanges(&pc, jp_data, 0, &r, 1);
    }
    stbtt_PackEnd(&pc);

    uint8_t *rgba = malloc((size_t)a->width * a->height * 4);
    if (!rgba)
    {
        free(alpha);
        return NULL;
    }
    for (int i = 0; i < a->width * a->height; ++i)
    {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = alpha[i];
    }
    a->texture = plat_texture_create(rgba, a->width, a->height, true);
    free(rgba);
    free(alpha);

    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font_info[weight], &asc, &desc, &gap);
    a->ascent = asc * stbtt_ScaleForPixelHeight(&font_info[weight], (float)px);
    a->weight = weight;
    a->px = px;
    ++atlas_count;
    return a;
}

/* Decodes one UTF-8 sequence. */
static int decode(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    int cp, len;
    if (s[0] < 0x80)
        cp = s[0], len = 1;
    else if ((s[0] & 0xe0) == 0xc0 && s[1])
        cp = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f), len = 2;
    else if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2])
        cp = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f), len = 3;
    else if ((s[0] & 0xf8) == 0xf0 && s[1] && s[2] && s[3])
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f),
        len = 4;
    else
        cp = s[0], len = 1; /* stray Latin-1 byte */
    *p += len;
    return cp;
}

static int next_cp(const char **p)
{
    return decode(p);
}

/* The packed glyph for a character: Latin-1, then the Japanese set, else '?'. */
static const stbtt_packedchar *atlas_glyph(const Atlas *a, int cp, const stbtt_packedchar **base,
                                           int *index)
{
    if (cp >= FIRST_CP && cp < FIRST_CP + CP_COUNT)
    {
        *base = a->chars;
        *index = cp - FIRST_CP;
        return &a->chars[*index];
    }
    if (a->extra && extra_count)
    {
        const int *hit = bsearch(&cp, extra_cp, (size_t)extra_count, sizeof(int), cmp_int);
        if (hit)
        {
            *base = a->extra;
            *index = (int)(hit - extra_cp);
            return &a->extra[*index];
        }
    }
    *base = a->chars;
    *index = '?' - FIRST_CP;
    return &a->chars[*index];
}

float text_width(float size, int weight, const char *s)
{
    Atlas *a = get_atlas(weight, size);
    if (!a)
        return 0.0f;
    float scale = size / (float)a->px, w = 0.0f;
    while (*s)
    {
        const stbtt_packedchar *base;
        int index;
        w += atlas_glyph(a, next_cp(&s), &base, &index)->xadvance;
    }
    return w * scale;
}

void text_draw(float x, float y, float size, int weight, uint32_t argb, int align, const char *s)
{
    Atlas *a = get_atlas(weight, size);
    if (!a || !a->texture || !*s)
        return;
    if (align != ALIGN_LEFT)
    {
        float w = text_width(size, weight, s);
        x -= align == ALIGN_CENTER ? w * 0.5f : w;
    }
    /* One blit per glyph: the software renderer's copy path is far cheaper
     * than textured triangles. */
    float scale = size / (float)a->px, pen = 0.0f;
    while (*s)
    {
        int cp = next_cp(&s);
        const stbtt_packedchar *base;
        int index;
        const stbtt_packedchar *g = atlas_glyph(a, cp, &base, &index);
        if (g->x1 > g->x0 && g->y1 > g->y0)
            plat_draw_texture_region(a->texture, g->x0, g->y0, g->x1 - g->x0, g->y1 - g->y0,
                                     x + (pen + g->xoff) * scale, y + (a->ascent + g->yoff) * scale,
                                     (g->xoff2 - g->xoff) * scale, (g->yoff2 - g->yoff) * scale,
                                     argb);
        pen += g->xadvance;
    }
}

void text_draw_fit(float x, float y, float size, int weight, uint32_t argb, int align,
                   float max_width, const char *s)
{
    if (text_width(size, weight, s) <= max_width)
    {
        text_draw(x, y, size, weight, argb, align, s);
        return;
    }
    char buf[256];
    size_t len = strlen(s);
    if (len >= sizeof(buf) - 4)
        len = sizeof(buf) - 4;
    while (len > 0)
    {
        --len;
        while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80)
            --len; /* stay on a UTF-8 boundary */
        memcpy(buf, s, len);
        strcpy(buf + len, "...");
        if (text_width(size, weight, buf) <= max_width)
            break;
    }
    text_draw(x, y, size, weight, argb, align, buf);
}

/* ---------------------------------------------------------------- CPU rendering */

/* Inter for Latin-1, the Japanese subset for what it has (at Inter's em size). */
static const stbtt_fontinfo *cpu_font(const stbtt_fontinfo *f, float scale, int *cp,
                                      float *glyph_scale)
{
    *glyph_scale = scale;
    if (*cp <= 0xff)
        return f;
    if (jp_ok && stbtt_FindGlyphIndex(&jp_info, *cp))
    {
        *glyph_scale = stbtt_ScaleForMappingEmToPixels(
            &jp_info, scale / stbtt_ScaleForMappingEmToPixels(f, 1.0f));
        return &jp_info;
    }
    *cp = '?';
    return f;
}

static float cpu_line_width(const stbtt_fontinfo *f, float scale, const char *s, size_t n)
{
    float w = 0.0f;
    const char *end = s + n;
    while (s < end && *s)
    {
        int adv, lsb, cp = next_cp(&s);
        float gs;
        const stbtt_fontinfo *g = cpu_font(f, scale, &cp, &gs);
        stbtt_GetCodepointHMetrics(g, cp, &adv, &lsb);
        w += adv * gs;
    }
    return w;
}

static void cpu_draw_line(uint8_t *rgba, int width, int height, float x, int baseline,
                          const stbtt_fontinfo *f, float scale, uint32_t argb, const char *s,
                          size_t n)
{
    const char *end = s + n;
    uint8_t cr = (argb >> 16) & 0xff, cg = (argb >> 8) & 0xff, cb = argb & 0xff;
    while (s < end && *s)
    {
        int cp = next_cp(&s);
        int adv, lsb, x0, y0, x1, y1;
        float gs;
        const stbtt_fontinfo *g = cpu_font(f, scale, &cp, &gs);
        stbtt_GetCodepointHMetrics(g, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(g, cp, gs, gs, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw < 512 && gh < 512)
        {
            static unsigned char glyph[512 * 512];
            stbtt_MakeCodepointBitmap(g, glyph, gw, gh, gw, gs, gs, cp);
            for (int gy = 0; gy < gh; ++gy)
                for (int gx = 0; gx < gw; ++gx)
                {
                    int px = (int)x + x0 + gx, py = baseline + y0 + gy;
                    if (px < 0 || py < 0 || px >= width || py >= height)
                        continue;
                    unsigned a = glyph[gy * gw + gx] * (argb >> 24) / 255;
                    uint8_t *d = &rgba[(py * width + px) * 4];
                    d[0] = (uint8_t)((cr * a + d[0] * (255 - a)) / 255);
                    d[1] = (uint8_t)((cg * a + d[1] * (255 - a)) / 255);
                    d[2] = (uint8_t)((cb * a + d[2] * (255 - a)) / 255);
                }
        }
        x += adv * gs;
    }
}

int text_render_rgba(uint8_t *rgba, int width, int height, int y, float size, int weight,
                     uint32_t argb, int max_width, const char *s)
{
    weight = weight ? FONT_BOLD : FONT_REGULAR;
    if (!font_ok[weight])
        return 0;
    const stbtt_fontinfo *f = &font_info[weight];
    float scale = stbtt_ScaleForPixelHeight(f, size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(f, &asc, &desc, &gap);
    int line_h = (int)((asc - desc + gap) * scale);
    int start_y = y;

    while (*s)
    {
        /* longest run of words that fits */
        size_t best = 0, i = 0;
        while (s[i])
        {
            size_t j = i;
            while (s[j] && s[j] != ' ')
                ++j;
            if (best > 0 && cpu_line_width(f, scale, s, j) > max_width)
                break;
            best = j;
            if (!s[j])
                break;
            i = j + 1;
        }
        if (best == 0)
            best = strlen(s);
        float lw = cpu_line_width(f, scale, s, best);
        cpu_draw_line(rgba, width, height, (width - lw) * 0.5f, y + (int)(asc * scale), f, scale,
                      argb, s, best);
        y += line_h;
        s += best;
        while (*s == ' ')
            ++s;
        if (y + line_h > height)
            break;
    }
    return y - start_y;
}
