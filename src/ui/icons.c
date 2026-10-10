/*
 * SwanStationPS5 - interface icons: a Tabler Icons subset (MIT), drawn like text.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * assets/fonts/tabler-SwanStationPS5.ttf holds only the icons in icons_list.h (made by
 * tools/make_icon_font.py). Each size used gets one small atlas.
 */
#include "icons.h"

#include "../platform/platform.h"
#include "stb_truetype.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_SIZES 10

typedef struct
{
    int px;
    int width, height;
    stbtt_packedchar chars[ICON_COUNT];
    PlatTexture *texture;
} IconAtlas;

static unsigned char *font;
static bool font_ok, tried;
static IconAtlas atlases[MAX_SIZES];
static int atlas_count;

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

static IconAtlas *atlas_for(float size)
{
    if (!tried)
    {
        tried = true;
        char path[SwanStationPS5_PATH_MAX];
        plat_asset_path(path, sizeof(path), "fonts/tabler-SwanStationPS5.ttf");
        font = read_file(path);
        stbtt_fontinfo info;
        font_ok = font && stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0));
        if (!font_ok)
            SwanStationPS5_log("icons: %s missing or invalid", path);
    }
    if (!font_ok)
        return NULL;
    int px = (int)(size + 0.5f);
    for (int i = 0; i < atlas_count; ++i)
        if (atlases[i].px == px)
            return &atlases[i];
    if (atlas_count == MAX_SIZES)
    {
        /* all the sizes are taken: the closest one, scaled by icon_draw */
        int best = 0;
        for (int i = 1; i < atlas_count; ++i)
            if (abs(atlases[i].px - px) < abs(atlases[best].px - px))
                best = i;
        return &atlases[best];
    }

    IconAtlas *a = &atlases[atlas_count];
    a->px = px;
    a->width = px <= 32 ? 256 : px <= 64 ? 512 : 1024;
    a->height = px <= 48 ? 256 : px <= 96 ? 512 : 1024;
    unsigned char *alpha = calloc((size_t)a->width * a->height, 1);
    uint8_t *rgba = malloc((size_t)a->width * a->height * 4);
    if (!alpha || !rgba)
    {
        free(alpha);
        free(rgba);
        return NULL;
    }
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, alpha, a->width, a->height, 0, 1, NULL);
    stbtt_PackSetOversampling(&pc, 1, 1);
    stbtt_pack_range r = {0};
    r.font_size = (float)px;
    r.array_of_unicode_codepoints = (int *)ICON_CODEPOINTS;
    r.num_chars = ICON_COUNT;
    r.chardata_for_range = a->chars;
    stbtt_PackFontRanges(&pc, font, 0, &r, 1);
    stbtt_PackEnd(&pc);
    for (int i = 0; i < a->width * a->height; ++i)
    {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = alpha[i];
    }
    a->texture = plat_texture_create(rgba, a->width, a->height, true);
    free(alpha);
    free(rgba);
    ++atlas_count;
    return a;
}

void icon_draw(int icon, float x, float y, float size, uint32_t argb)
{
    IconAtlas *a = atlas_for(size);
    if (!a || !a->texture)
        return;
    int index = -1;
    for (int i = 0; i < ICON_COUNT; ++i)
        if (ICON_CODEPOINTS[i] == icon)
            index = i;
    if (index < 0)
        return;
    const stbtt_packedchar *g = &a->chars[index];
    /* the atlas of the closest size, scaled to the one asked for */
    const float k = size / (float)a->px;
    float gw = (g->xoff2 - g->xoff) * k, gh = (g->yoff2 - g->yoff) * k;
    if (gw <= 0 || gh <= 0)
        return;
    /* centred in the size x size box */
    plat_draw_texture_region(a->texture, g->x0, g->y0, g->x1 - g->x0, g->y1 - g->y0,
                             x + (size - gw) * 0.5f, y + (size - gh) * 0.5f, gw, gh, argb);
}

void icons_shutdown(void)
{
    for (int i = 0; i < atlas_count; ++i)
        plat_texture_free(atlases[i].texture);
    atlas_count = 0;
    free(font);
    font = NULL;
    tried = false;
}
