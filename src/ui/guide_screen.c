/*
 * PSXS5 - text guides: walkthroughs and FAQs (.txt) you put beside a game,
 * read in the game from the PSXS5 menu.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every .txt in the game's folder (but serial.txt) or in its guides/ folder
 * is a guide; a sandboxed PSXS5 can't list folders, so it looks for guide.txt
 * and guide1.txt to guide9.txt. Text is shown in a monospace face so the ASCII
 * tables of GameFAQs-style guides line up, and the place you were reading is
 * kept per guide (<guide>.txt.pos beside it, or in the states folder).
 */
#include "../app.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GUIDES 32
#define MAX_BYTES (6 * 1024 * 1024)
#define WRAP 96 /* columns */

static struct
{
    const Game *scanned_for;
    int count;
    char paths[MAX_GUIDES][PSXS5_PATH_MAX];
    int chosen;           /* -1: the list of guides */
    int list_cursor;
    char *text;           /* the open guide, wrapped: lines end in '\0' */
    int *lines;           /* offsets of each line in text */
    int line_count;
    float top, top_target; /* first line shown */
} G;

static bool opens(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static void add(const char *path)
{
    if (G.count < MAX_GUIDES)
        str_copy(G.paths[G.count++], sizeof(G.paths[0]), path);
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (!str_icmp(path_ext(e->d_name), "txt") && str_icmp(e->d_name, "serial.txt") != 0 && e->d_name[0] != '.')
        {
            char path[PSXS5_PATH_MAX];
            path_join(path, sizeof(path), dir, e->d_name);
            add(path);
        }
    closedir(d);
}

static int by_name(const void *a, const void *b)
{
    return str_icmp((const char *)a, (const char *)b);
}

static void scan(void)
{
    if (G.scanned_for == app.game)
        return;
    G.scanned_for = app.game;
    G.count = 0;
    if (!app.game || !app.game->folder[0])
        return;
    char guides[PSXS5_PATH_MAX];
    path_join(guides, sizeof(guides), app.game->folder, "guides");
    if (!app.sandboxed)
    {
        scan_dir(app.game->folder);
        scan_dir(guides);
    }
    else
        for (int i = 0; i <= 9; ++i)
        {
            char name[32], path[PSXS5_PATH_MAX];
            snprintf(name, sizeof(name), i ? "guide%d.txt" : "guide.txt", i);
            path_join(path, sizeof(path), app.game->folder, name);
            if (opens(path))
                add(path);
        }
    qsort(G.paths, (size_t)G.count, sizeof(G.paths[0]), by_name);
}

int guide_count(void)
{
    scan();
    return G.count;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* where the reading position of a guide is kept */
static void pos_path(int guide, char *out, size_t size)
{
    char file[200];
    snprintf(file, sizeof(file), "%.60s.%.100s.pos", app.game ? app.game->id : "game", base_name(G.paths[guide]));
    path_join(out, size, app.paths.states, file);
}

static void save_position(void)
{
    if (G.chosen < 0 || !G.text)
        return;
    char path[PSXS5_PATH_MAX];
    pos_path(G.chosen, path, sizeof(path));
    make_dirs(app.paths.states);
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%d\n", (int)G.top_target);
        fclose(f);
    }
}

static void close_guide(void)
{
    save_position();
    free(G.text);
    free(G.lines);
    G.text = NULL;
    G.lines = NULL;
    G.line_count = 0;
}

/* Reads the guide and wraps it at WRAP columns (tabs to 8). */
static bool open_guide(int guide)
{
    close_guide();
    FILE *f = fopen(G.paths[guide], "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0)
        size = 0;
    if (size > MAX_BYTES)
        size = MAX_BYTES;
    char *raw = malloc((size_t)size + 1);
    if (!raw)
    {
        fclose(f);
        return false;
    }
    size = (long)fread(raw, 1, (size_t)size, f);
    fclose(f);
    raw[size] = '\0';
    /* worst case: each line grows by its tabs and wraps */
    size_t cap = (size_t)size * 2 + 1024;
    G.text = malloc(cap);
    int line_cap = 1024;
    G.lines = malloc(sizeof(int) * (size_t)line_cap);
    if (!G.text || !G.lines)
    {
        free(raw);
        close_guide();
        return false;
    }
    size_t w = 0;
    int col = 0;
    G.line_count = 0;
    G.lines[G.line_count++] = 0;
    const char *p = raw;
    if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf)
        p += 3; /* UTF-8 mark */
    for (; *p && w + 16 < cap; ++p)
    {
        bool newline = *p == '\n';
        if (*p == '\r')
            continue;
        if (!newline && col >= WRAP)
            newline = true, --p; /* wrap, then take this character again */
        if (newline)
        {
            G.text[w++] = '\0';
            col = 0;
            if (G.line_count == line_cap)
            {
                int *more = realloc(G.lines, sizeof(int) * (size_t)line_cap * 2);
                if (!more)
                    break;
                G.lines = more;
                line_cap *= 2;
            }
            G.lines[G.line_count++] = (int)w;
            continue;
        }
        if (*p == '\t')
        {
            do
                G.text[w++] = ' ';
            while (++col % 8 && col < WRAP);
            continue;
        }
        G.text[w++] = *p;
        /* UTF-8 continuation bytes don't take a column */
        if (((unsigned char)*p & 0xc0) != 0x80)
            ++col;
    }
    G.text[w] = '\0';
    free(raw);
    G.chosen = guide;
    G.top = G.top_target = 0;
    char path[PSXS5_PATH_MAX];
    pos_path(guide, path, sizeof(path));
    FILE *pf = fopen(path, "r");
    if (pf)
    {
        int line = 0;
        if (fscanf(pf, "%d", &line) == 1 && line > 0 && line < G.line_count)
            G.top = G.top_target = (float)line;
        fclose(pf);
    }
    return true;
}

void guide_open(void)
{
    scan();
    G.chosen = -1;
    G.list_cursor = 0;
    if (G.count == 1 && !open_guide(0))
    {
        app_toast("Could not open the guide");
        return;
    }
    app.screen = SCREEN_GUIDE;
}

void guide_screen(uint32_t pressed)
{
    app_draw_game(40);
    draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);

    if (G.chosen < 0)
    {
        /* ---------------------------------------------- which guide */
        if (pressed & BIT(BTN_UP))
            G.list_cursor = (G.list_cursor + G.count - 1) % (G.count ? G.count : 1);
        if (pressed & BIT(BTN_DOWN))
            G.list_cursor = (G.list_cursor + 1) % (G.count ? G.count : 1);
        if (pressed & (BIT(BTN_UP) | BIT(BTN_DOWN)))
            sfx_play(SFX_CLICK);
        if (pressed & BIT(BTN_CIRCLE))
        {
            sfx_play(SFX_BACK);
            app.screen = SCREEN_MENU;
            return;
        }
        if ((pressed & BIT(BTN_CROSS)) && G.count)
        {
            sfx_play(SFX_SELECT);
            if (!open_guide(G.list_cursor))
                app_toast("Could not open the guide");
            return;
        }
        text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Guides"));
        const float x = 360, w = plat_width() - 720.0f, top = 170, row_h = 64;
        draw_rrect(x - 16, top - 16, w + 32, G.count * row_h + 24, TH_RADIUS, TH_CARD);
        for (int i = 0; i < G.count; ++i)
        {
            float y = top + i * row_h;
            bool on = i == G.list_cursor;
            if (on)
                draw_rrect(x, y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
            icon_draw(ICON_FILE_TEXT, x + 20, y + 13, 30, TH_FOCUS);
            text_draw_fit(x + 70, y + 15, 24, on ? FONT_BOLD : FONT_REGULAR, on ? TH_TEXT : TH_TEXT_SOFT, ALIGN_LEFT,
                          w - 100, base_name(G.paths[i]));
        }
        static const int glyphs[] = {GLYPH_CROSS, GLYPH_CIRCLE};
        static const char *const labels[] = {"Read", "Back"};
        app_draw_hints(glyphs, labels, 2, NULL);
        app_draw_toast();
        return;
    }

    /* -------------------------------------------------- reading */
    const float size = 24, line_h = 32, top_y = 150, bottom_y = plat_height() - 110.0f;
    int visible = (int)((bottom_y - top_y) / line_h);
    float max_top = (float)(G.line_count - visible > 0 ? G.line_count - visible : 0);
    if (pressed & BIT(BTN_DOWN))
        G.top_target += 1;
    if (pressed & BIT(BTN_UP))
        G.top_target -= 1;
    if (pressed & BIT(BTN_R1))
        G.top_target += visible - 2;
    if (pressed & BIT(BTN_L1))
        G.top_target -= visible - 2;
    if (pressed & BIT(BTN_R2))
        G.top_target += visible * 10;
    if (pressed & BIT(BTN_L2))
        G.top_target -= visible * 10;
    /* the right stick scrolls smoothly, faster the further it's pushed */
    float stick = app.pads[0].ry / 32767.0f;
    if (stick > 0.2f || stick < -0.2f)
        G.top_target += stick * stick * stick * app.dt * 90.0f;
    if (pressed & BIT(BTN_TRIANGLE))
        G.top_target = 0;
    G.top_target = G.top_target < 0 ? 0 : G.top_target > max_top ? max_top : G.top_target;
    anim_approach(&G.top, G.top_target, app.dt, TH_SNAP);
    if (pressed & BIT(BTN_CIRCLE))
    {
        sfx_play(SFX_BACK);
        close_guide();
        if (G.count > 1)
            G.chosen = -1; /* back to the list */
        else
            app.screen = SCREEN_MENU;
        return;
    }

    text_draw_fit(TH_MARGIN, 40, 36, FONT_BOLD, TH_TEXT, ALIGN_LEFT, plat_width() - 600.0f, base_name(G.paths[G.chosen]));
    char where[64];
    int shown_line = (int)(G.top_target + 0.5f);
    snprintf(where, sizeof(where), tr("Line %d of %d"), shown_line + 1, G.line_count);
    text_draw(plat_width() - TH_MARGIN, 50, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, where);

    float cw = text_width(size, FONT_MONO, "M") * WRAP + 64;
    if (cw > plat_width() - 2 * TH_MARGIN)
        cw = plat_width() - 2 * TH_MARGIN;
    float x = (plat_width() - cw) * 0.5f;
    draw_rrect(x, top_y - 24, cw, bottom_y - top_y + 40, TH_RADIUS, TH_CARD);
    plat_set_clip((int)x, (int)top_y - 8, (int)cw, (int)(bottom_y - top_y + 12));
    int first = (int)G.top;
    float frac = G.top - first;
    for (int i = 0; i <= visible && first + i < G.line_count; ++i)
    {
        const char *s = G.text + G.lines[first + i];
        if (*s)
            text_draw(x + 32, top_y + (i - frac) * line_h, size, FONT_MONO, TH_TEXT_SOFT, ALIGN_LEFT, s);
    }
    plat_set_clip(0, 0, 0, 0);
    /* where you are in the whole guide */
    if (G.line_count > visible)
    {
        float track = bottom_y - top_y, knob = track * visible / G.line_count;
        if (knob < 30)
            knob = 30;
        float ky = top_y + (track - knob) * (max_top > 0 ? G.top / max_top : 0);
        draw_rrect(x + cw - 14, top_y, 6, track, 3, TH_DIVIDER);
        draw_rrect(x + cw - 14, ky, 6, knob, 3, TH_FOCUS);
    }

    static const int glyphs[] = {GLYPH_UP, GLYPH_TRIANGLE, GLYPH_CIRCLE};
    static const char *const labels[] = {"Scroll", "Top", "Back"};
    app_draw_hints(glyphs, labels, 3, NULL);
    const HintCombo right[2] = {{{GLYPH_L1, GLYPH_R1}, 2, '/', "Page"}, {{GLYPH_L2, GLYPH_R2}, 2, '/', "Jump"}};
    app_draw_hints_right(right, 2);
    app_draw_toast();
}
