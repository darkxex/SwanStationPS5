/*
 * SwanStationPS5 - memory card manager: every game's card, the saves on it, export
 * for other emulators, import from /data/SwanStationPS5/import.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The core keeps one card per game: <saves>/<serial>_1.mcd (raw, 128 KiB).
 * Imports accept raw cards (.mcr .mcd .srm, DuckStation/ePSXe/RetroArch) and
 * DexDrive .gme files; the card they replace is kept as .bak.
 */
#include "../app.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "coverflow.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARD_SIZE (128 * 1024)
#define MAX_CARDS 256
#define MAX_IMPORTS 64

typedef struct
{
    char serial[24];
    char title[96];
    char path[SwanStationPS5_PATH_MAX];
    int used;            /* blocks in use */
    char saves[15][48];  /* one name per save */
    int save_blocks[15];
    int save_count;
} Card;

static struct
{
    Card cards[MAX_CARDS];
    int count, cursor;
    float scroll, sel_y;
    enum Screen back_to;
    bool picking; /* choosing a file to import */
    char imports[MAX_IMPORTS][128];
    int import_count, import_cursor;
} C;

/* Save titles are Shift-JIS; most use full-width ASCII, which this maps back. */
static void sjis_to_ascii(const uint8_t *in, size_t len, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; i < len && in[i] && o + 1 < size;)
    {
        uint8_t c = in[i];
        if (c < 0x80)
        {
            out[o++] = (char)c;
            ++i;
            continue;
        }
        if (i + 1 >= len)
            break;
        unsigned code = (unsigned)c << 8 | in[i + 1];
        char ch = '?';
        if (code == 0x8140)
            ch = ' ';
        else if (code >= 0x824f && code <= 0x8258)
            ch = (char)('0' + code - 0x824f);
        else if (code >= 0x8260 && code <= 0x8279)
            ch = (char)('A' + code - 0x8260);
        else if (code >= 0x8281 && code <= 0x829a)
            ch = (char)('a' + code - 0x8281);
        else if (code == 0x8146)
            ch = ':';
        else if (code == 0x815e)
            ch = '/';
        else if (code == 0x817c || code == 0x815d)
            ch = '-';
        else if (code == 0x8144)
            ch = '.';
        else if (code == 0x8169)
            ch = '(';
        else if (code == 0x816a)
            ch = ')';
        out[o++] = ch;
        i += 2;
    }
    while (o > 0 && out[o - 1] == ' ')
        --o;
    out[o] = '\0';
}

static void read_card(Card *c)
{
    c->used = 0;
    c->save_count = 0;
    FILE *f = fopen(c->path, "rb");
    if (!f)
        return;
    static uint8_t data[CARD_SIZE];
    size_t n = fread(data, 1, CARD_SIZE, f);
    fclose(f);
    if (n != CARD_SIZE || data[0] != 'M' || data[1] != 'C')
        return;
    for (int b = 1; b <= 15; ++b)
    {
        const uint8_t *frame = data + b * 0x80;
        uint8_t state = frame[0];
        if (state == 0x51 || state == 0x52 || state == 0x53)
            ++c->used;
        if (state != 0x51)
            continue; /* only first blocks start a save */
        int blocks = (int)((frame[4] | frame[5] << 8 | frame[6] << 16) / 0x2000);
        const uint8_t *title = data + b * 0x2000 + 4; /* the save's own header: "SC", then the title */
        char name[48];
        if (data[b * 0x2000] == 'S' && data[b * 0x2000 + 1] == 'C')
            sjis_to_ascii(title, 64, name, sizeof(name));
        else
            snprintf(name, sizeof(name), "%.20s", (const char *)frame + 0x0a);
        str_copy(c->saves[c->save_count], sizeof(c->saves[0]), name[0] ? name : "?");
        c->save_blocks[c->save_count++] = blocks > 0 ? blocks : 1;
    }
}

static void scan(void)
{
    C.count = 0;
    DIR *d = opendir(app.paths.saves);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && C.count < MAX_CARDS)
    {
        size_t len = strlen(e->d_name);
        if (len < 7 || strcmp(e->d_name + len - 6, "_1.mcd") != 0)
            continue;
        Card *c = &C.cards[C.count++];
        snprintf(c->serial, sizeof(c->serial), "%.*s", (int)(len - 6), e->d_name);
        path_join(c->path, sizeof(c->path), app.paths.saves, e->d_name);
        str_copy(c->title, sizeof(c->title), c->serial);
        for (int i = 0; i < app.library.count; ++i)
            if (!str_icmp(app.library.games[i].serial, c->serial))
                str_copy(c->title, sizeof(c->title), shelf_game_title(&app.library.games[i]));
        read_card(c);
    }
    closedir(d);
    /* by title */
    for (int a = 0; a < C.count; ++a)
        for (int b = a + 1; b < C.count; ++b)
            if (str_icmp(C.cards[b].title, C.cards[a].title) < 0)
            {
                Card t = C.cards[a];
                C.cards[a] = C.cards[b];
                C.cards[b] = t;
            }
}

void memcards_open(enum Screen back_to)
{
    C.back_to = back_to;
    C.cursor = 0;
    C.scroll = 0;
    C.sel_y = 0;
    C.picking = false;
    scan();
    app.screen = SCREEN_MEMCARDS;
}

static bool copy_file(const char *from, const char *to, long skip)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return false;
    static uint8_t data[CARD_SIZE];
    fseek(in, skip, SEEK_SET);
    bool ok = fread(data, 1, CARD_SIZE, in) == CARD_SIZE;
    fclose(in);
    if (!ok || data[0] != 'M' || data[1] != 'C')
        return false;
    char temp[SwanStationPS5_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", to);
    FILE *out = fopen(temp, "wb");
    ok = out && fwrite(data, 1, CARD_SIZE, out) == CARD_SIZE;
    if (out)
        ok = fclose(out) == 0 && ok;
    return ok && rename(temp, to) == 0;
}

static void export_card(const Card *c)
{
    char dir[SwanStationPS5_PATH_MAX], to[SwanStationPS5_PATH_MAX], file[64];
    path_join(dir, sizeof(dir), app.paths.root, "memcards-export");
    make_dirs(dir);
    snprintf(file, sizeof(file), "%s.mcd", c->serial);
    path_join(to, sizeof(to), dir, file);
    char msg[SwanStationPS5_PATH_MAX + 32];
    if (copy_file(c->path, to, 0))
        snprintf(msg, sizeof(msg), tr("Exported to %s"), to);
    else
        str_copy(msg, sizeof(msg), tr("Export failed"));
    app_toast(msg);
}

static void list_imports(void)
{
    C.import_count = 0;
    char dir[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "import");
    make_dirs(dir);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && C.import_count < MAX_IMPORTS)
    {
        const char *ext = path_ext(e->d_name);
        if (!str_icmp(ext, "mcr") || !str_icmp(ext, "mcd") || !str_icmp(ext, "srm") ||
            !str_icmp(ext, "gme"))
            str_copy(C.imports[C.import_count++], sizeof(C.imports[0]), e->d_name);
    }
    closedir(d);
}

static void import_card(Card *c, const char *name)
{
    char dir[SwanStationPS5_PATH_MAX], from[SwanStationPS5_PATH_MAX], backup[SwanStationPS5_PATH_MAX + 8];
    path_join(dir, sizeof(dir), app.paths.root, "import");
    path_join(from, sizeof(from), dir, name);
    snprintf(backup, sizeof(backup), "%s.bak", c->path);
    copy_file(c->path, backup, 0); /* keep the card it replaces */
    long skip = !str_icmp(path_ext(name), "gme") ? 0xF40 : 0; /* DexDrive header */
    if (copy_file(from, c->path, skip))
    {
        read_card(c);
        app_toast("Memory card imported (the old one is kept as .bak)");
    }
    else
        app_toast("That file isn't a PS1 memory card");
}

void memcards_screen(uint32_t pressed)
{
    if (C.picking)
    {
        if (pressed & BIT(BTN_UP) && C.import_count)
            C.import_cursor = (C.import_cursor + C.import_count - 1) % C.import_count;
        if (pressed & BIT(BTN_DOWN) && C.import_count)
            C.import_cursor = (C.import_cursor + 1) % C.import_count;
        if ((pressed & BIT(BTN_CROSS)) && C.import_count && C.count)
        {
            import_card(&C.cards[C.cursor], C.imports[C.import_cursor]);
            C.picking = false;
        }
        if (pressed & BIT(BTN_CIRCLE))
            C.picking = false;
        pressed = 0;
    }
    if (C.count)
    {
        int before = C.cursor;
        if (pressed & BIT(BTN_UP))
            C.cursor = (C.cursor + C.count - 1) % C.count;
        if (pressed & BIT(BTN_DOWN))
            C.cursor = (C.cursor + 1) % C.count;
        if (C.cursor != before)
            sfx_play(SFX_CLICK);
        if (pressed & BIT(BTN_CROSS))
        {
            export_card(&C.cards[C.cursor]);
            sfx_play(SFX_SELECT);
        }
        if (pressed & BIT(BTN_SQUARE))
        {
            if (app.game)
                app_toast("Quit the game first");
            else
            {
                list_imports();
                C.import_cursor = 0;
                C.picking = true;
                sfx_play(SFX_SELECT);
            }
        }
    }
    if (pressed & BIT(BTN_CIRCLE))
    {
        sfx_play(SFX_BACK);
        app.screen = C.back_to;
        return;
    }

    if (app.game)
    {
        app_draw_game(40);
        draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);
    }
    else
        shelf_backdrop();
    text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Memory cards"));
    text_draw(TH_MARGIN + 2, 100, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT,
              tr("One card per game. Import from /data/SwanStationPS5/import."));

    /* the cards, on the left */
    const int rows = 10;
    const float x = TH_MARGIN, w = 760, top = 170, row_h = 76;
    draw_rrect(x - 8, top - 8, w + 16, rows * row_h + 16, TH_RADIUS, TH_CARD);
    static int top_row;
    top_row = list_top_row(top_row, C.cursor, rows, C.count);
    anim_approach(&C.scroll, (float)top_row, app.dt, TH_SNAP);
    plat_set_clip((int)x - 4, (int)top - 4, (int)w + 8, (int)(rows * row_h) + 4);
    if (C.count)
    {
        float target = top + (C.cursor - C.scroll) * row_h;
        if (C.sel_y == 0)
            C.sel_y = target;
        anim_approach(&C.sel_y, target, app.dt, TH_SNAP * 1.5f);
        draw_rrect(x, C.sel_y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
        draw_rrect_outline(x, C.sel_y, w, row_h - 8, TH_RADIUS_SMALL, 3, TH_FOCUS);
    }
    int first = (int)floorf(C.scroll);
    for (int i = first; i < C.count && i <= first + rows; ++i)
    {
        const Card *c = &C.cards[i];
        float y = top + (i - C.scroll) * row_h;
        icon_draw(ICON_DEVICE_FLOPPY, x + 20, y + 18, 32, TH_FOCUS);
        text_draw_fit(x + 70, y + 8, 24, FONT_BOLD, TH_TEXT, ALIGN_LEFT, w - 230, c->title);
        text_draw(x + 70, y + 40, 18, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, c->serial);
        char used[32];
        snprintf(used, sizeof(used), tr("%d / 15 blocks"), c->used);
        text_draw(x + w - 20, y + 22, 20, FONT_REGULAR, TH_TEXT_SOFT, ALIGN_RIGHT, used);
    }
    plat_set_clip(0, 0, 0, 0);
    if (!C.count)
        text_draw(x + w * 0.5f, top + 300, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                  tr("No memory cards yet: they appear once a game saves"));

    /* the selected card: its saves as a block map and a list */
    const float px = x + w + 40, pw = plat_width() - TH_MARGIN - px;
    draw_rrect(px, top - 8, pw, rows * row_h + 16, TH_RADIUS, TH_CARD);
    if (C.count)
    {
        const Card *c = &C.cards[C.cursor];
        text_draw_fit(px + 32, top + 20, 28, FONT_BOLD, TH_TEXT, ALIGN_LEFT, pw - 64, c->title);
        float bx = px + 32, by = top + 80, bs = (pw - 64 - 14 * 8) / 15.0f;
        int block = 0;
        const uint32_t hues[] = {TH_SWITCH_ON, TH_GOLD, TH_GOOD, 0xffff8f8fu, 0xffc07cffu};
        for (int s = 0; s < c->save_count; ++s)
            for (int k = 0; k < c->save_blocks[s] && block < 15; ++k, ++block)
                draw_rrect(bx + block * (bs + 8), by, bs, bs, 6, hues[s % 5]);
        for (; block < 15; ++block)
            draw_rrect(bx + block * (bs + 8), by, bs, bs, 6, TH_PILL);
        for (int s = 0; s < c->save_count && s < 12; ++s)
        {
            float sy = by + bs + 40 + s * 46;
            draw_rrect(px + 32, sy + 6, 14, 14, 4, hues[s % 5]);
            text_draw_fit(px + 60, sy, 22, FONT_REGULAR, TH_TEXT, ALIGN_LEFT, pw - 200, c->saves[s]);
            char blocks[24];
            snprintf(blocks, sizeof(blocks), tr(c->save_blocks[s] == 1 ? "%d block" : "%d blocks"),
                     c->save_blocks[s]);
            text_draw(px + pw - 32, sy, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, blocks);
        }
        if (!c->save_count)
            text_draw(px + 32, by + bs + 40, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, tr("This card is empty"));
    }

    if (C.picking)
    {
        draw_rect(0, 0, plat_width(), plat_height(), 0xc0000000u);
        const float dw = 900, dh = 600, dx = (plat_width() - dw) * 0.5f, dy = 220;
        draw_rrect(dx, dy, dw, dh, TH_RADIUS, TH_CARD);
        text_draw(dx + 40, dy + 30, 30, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Import a memory card"));
        if (!C.import_count)
        {
            char hint[SwanStationPS5_PATH_MAX + 64];
            snprintf(hint, sizeof(hint), tr("Put .mcr, .mcd, .srm or .gme files in %s/import"), app.paths.root);
            text_draw_fit(dx + 40, dy + 110, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, dw - 80, hint);
        }
        for (int i = 0; i < C.import_count && i < 9; ++i)
        {
            float iy = dy + 100 + i * 52;
            if (i == C.import_cursor)
                draw_rrect(dx + 24, iy - 6, dw - 48, 46, 10, TH_ROW_SELECTED);
            text_draw_fit(dx + 48, iy, 24, FONT_REGULAR, TH_TEXT, ALIGN_LEFT, dw - 96, C.imports[i]);
        }
        char into[160];
        snprintf(into, sizeof(into), tr("Replaces the card of %s"), C.count ? C.cards[C.cursor].title : "");
        text_draw_fit(dx + 40, dy + dh - 50, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, dw - 80, into);
    }

    static const int glyphs[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_CIRCLE};
    static const char *const labels[] = {"Export", "Import", "Back"};
    static const char *const pick_labels[] = {"Import", "Back"};
    static const int pick_glyphs[] = {GLYPH_CROSS, GLYPH_CIRCLE};
    if (C.picking)
        app_draw_hints(pick_glyphs, pick_labels, 2, NULL);
    else
        app_draw_hints(glyphs, labels, 3, NULL);
    app_draw_toast();
}
