/*
 * SwanStationPS5 - a game's manual: page images you put in the game's folder.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * <game folder>/manual/ holds one image per page (.jpg or .png). With /data
 * unlocked every image there is a page, in name order; a sandboxed SwanStationPS5
 * can't list folders, so it looks for numbered pages: 1.jpg, 2.jpg... (also
 * 01, 001 and .png). SwanStationPS5 never downloads manuals.
 */
#include "../app.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "stb_image.h"
#include "text.h"
#include "theme.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PAGES 400

static struct
{
    const Game *scanned_for;
    int count;
    char (*pages)[SwanStationPS5_PATH_MAX];
    int page;
    PlatTexture *texture;
    int loaded_page;
    bool zoom;
    float scroll, scroll_target;
} N;

static bool is_image(const char *name)
{
    return !str_icmp(path_ext(name), "jpg") || !str_icmp(path_ext(name), "jpeg") || !str_icmp(path_ext(name), "png");
}

static int by_name(const void *a, const void *b)
{
    /* "2.jpg" before "10.jpg": compare the numbers when both start with one */
    const char *x = strrchr((const char *)a, '/'), *y = strrchr((const char *)b, '/');
    x = x ? x + 1 : (const char *)a;
    y = y ? y + 1 : (const char *)b;
    long nx = strtol(x, NULL, 10), ny = strtol(y, NULL, 10);
    if (nx != ny && (nx || ny))
        return nx < ny ? -1 : 1;
    return str_icmp(x, y);
}

static bool opens(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static void scan(const Game *g)
{
    N.scanned_for = g;
    N.count = 0;
    if (!g || !g->folder[0])
        return;
    if (!N.pages && !(N.pages = malloc(sizeof(*N.pages) * MAX_PAGES)))
        return;
    char dir[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), g->folder, "manual");
    DIR *d = opendir(dir);
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)) && N.count < MAX_PAGES)
            if (e->d_name[0] != '.' && is_image(e->d_name))
                path_join(N.pages[N.count++], SwanStationPS5_PATH_MAX, dir, e->d_name);
        closedir(d);
        qsort(N.pages, (size_t)N.count, sizeof(*N.pages), by_name);
        return;
    }
    /* sandboxed: numbered pages, until one is missing */
    static const char *const forms[] = {"%d.jpg", "%d.png", "%02d.jpg", "%02d.png", "%03d.jpg", "%03d.png"};
    for (int n = 1; n <= MAX_PAGES; ++n)
    {
        bool found = false;
        for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]) && !found; ++f)
        {
            char name[16];
            snprintf(name, sizeof(name), forms[f], n);
            path_join(N.pages[N.count], SwanStationPS5_PATH_MAX, dir, name);
            found = opens(N.pages[N.count]);
        }
        if (!found)
            break;
        ++N.count;
    }
}

int manual_page_count(void)
{
    if (N.scanned_for != app.game)
        scan(app.game);
    return N.count;
}

void manual_open(void)
{
    if (!manual_page_count())
        return;
    N.page = 0;
    N.loaded_page = -1;
    N.zoom = false;
    N.scroll = N.scroll_target = 0;
    app.screen = SCREEN_MANUAL;
}

static void load_page(void)
{
    if (N.loaded_page == N.page)
        return;
    plat_texture_free(N.texture);
    N.texture = NULL;
    N.loaded_page = N.page;
    int w, h, comp;
    unsigned char *px = stbi_load(N.pages[N.page], &w, &h, &comp, 4);
    if (!px)
        return;
    N.texture = plat_texture_create(px, w, h, true);
    stbi_image_free(px);
}

void manual_screen(uint32_t pressed)
{
    if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_MENU)))
    {
        plat_texture_free(N.texture);
        N.texture = NULL;
        N.loaded_page = -1;
        sfx_play(SFX_BACK);
        app.screen = SCREEN_MENU;
        return;
    }
    int before = N.page;
    if (pressed & (BIT(BTN_RIGHT) | BIT(BTN_R1)))
        N.page = N.page + 1 < N.count ? N.page + 1 : N.page;
    if (pressed & (BIT(BTN_LEFT) | BIT(BTN_L1)))
        N.page = N.page > 0 ? N.page - 1 : 0;
    if (N.page != before)
    {
        N.scroll = N.scroll_target = 0;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_CROSS))
    {
        N.zoom = !N.zoom;
        N.scroll = N.scroll_target = 0;
        sfx_play(SFX_SELECT);
    }
    load_page();

    draw_rect(0, 0, plat_width(), plat_height(), TH_BG_DEEP);
    const float top = 24, bottom = 90, avail_h = plat_height() - top - bottom, avail_w = plat_width() - 48;
    if (N.texture)
    {
        int iw, ih;
        plat_texture_size(N.texture, &iw, &ih);
        float k = N.zoom ? avail_w / iw : (avail_h / ih < avail_w / iw ? avail_h / ih : avail_w / iw);
        float dw = iw * k, dh = ih * k;
        float max_scroll = dh > avail_h ? dh - avail_h : 0;
        if (pressed & BIT(BTN_DOWN))
            N.scroll_target += avail_h * 0.5f;
        if (pressed & BIT(BTN_UP))
            N.scroll_target -= avail_h * 0.5f;
        N.scroll_target = N.scroll_target < 0 ? 0 : N.scroll_target > max_scroll ? max_scroll : N.scroll_target;
        anim_approach(&N.scroll, N.scroll_target, app.dt, TH_SNAP);
        plat_set_clip(0, (int)top, plat_width(), (int)avail_h);
        plat_draw_texture(N.texture, (plat_width() - dw) * 0.5f, top + (dh < avail_h ? (avail_h - dh) * 0.5f : -N.scroll),
                          dw, dh, 0xffffffffu, false);
        plat_set_clip(0, 0, 0, 0);
    }
    else
        text_draw(plat_width() * 0.5f, plat_height() * 0.5f, 26, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                  tr("Can't read this page"));

    char where[48];
    snprintf(where, sizeof(where), tr("Page %d of %d"), N.page + 1, N.count);
    static const int glyphs[] = {GLYPH_LEFT, GLYPH_RIGHT, GLYPH_CROSS, GLYPH_CIRCLE};
    static const char *const labels[] = {"Previous", "Next", "Zoom", "Back"};
    app_draw_hints(glyphs, labels, 4, where);
}
