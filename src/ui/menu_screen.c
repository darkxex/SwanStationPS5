/*
 * SwanStationPS5 - the in-game menu (a side panel over the paused game) and cheats.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../core/host.h"
#include "../covers.h"
#include "../i18n.h"
#include "../net.h"
#include "../play.h"
#include "../platform/platform.h"
#include "../ra/achievements.h"
#include "coverflow.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"

#include "stb_image.h"

#include <SDL2/SDL.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum Item
{
    MI_RESUME,
    MI_SAVE,
    MI_LOAD,
    MI_AUTO,
    MI_DISC,
    MI_CHEATS,
    MI_ACHIEVEMENTS,
    MI_MANUAL,
    MI_GUIDE,
    MI_SETTINGS,
    MI_RESET,
    MI_QUIT,
    MI_COUNT
};

/* slot thumbnails, loaded when the menu opens */
static PlatTexture *thumbs[10];
static bool thumbs_loaded[10];

static void forget_thumbs(void)
{
    for (int s = 0; s < 10; ++s)
    {
        plat_texture_free(thumbs[s]);
        thumbs[s] = NULL;
        thumbs_loaded[s] = false;
    }
}

static PlatTexture *slot_thumb(int slot)
{
    if (!thumbs_loaded[slot])
    {
        thumbs_loaded[slot] = true;
        static uint8_t rgba[THUMB_W * THUMB_H * 4];
        char path[SwanStationPS5_PATH_MAX];
        app_state_path(path, sizeof(path), slot);
        if (play_load_thumb(path, rgba))
            thumbs[slot] = plat_texture_create(rgba, THUMB_W, THUMB_H, true);
    }
    return thumbs[slot];
}

static struct
{
    int cursor;
    float sel_y, open_t, slot_x;
    int disc_choice;
    int auto_choice; /* which auto-save, newest first */
    int cheat_cursor;
    float cheat_y, cheat_scroll;
} M;

void menu_open(void)
{
    M.cursor = MI_RESUME;
    M.open_t = 0;
    M.sel_y = 0;
    M.disc_choice = host_disc_index();
    M.auto_choice = 0;
    forget_thumbs();
    play_save_resume(true); /* the menu is a good moment: nothing is moving */
    app.screen = SCREEN_MENU;
    sfx_play(SFX_SELECT);
}

static bool item_shown(int i)
{
    int unlocked, total;
    if (i == MI_MANUAL)
        return manual_page_count() > 0;
    if (i == MI_GUIDE)
        return guide_count() > 0;
    if (i == MI_AUTO)
    {
        int slots[AUTO_SLOTS];
        long ages[AUTO_SLOTS];
        return play_auto_list(slots, ages) > 0;
    }
    if (i == MI_ACHIEVEMENTS)
        return ra_game_progress(&unlocked, &total);
    return i != MI_DISC || host_disc_count() > 1;
}

/* "2 min ago", "Yesterday"...; "" when the slot is empty */
static void slot_age(int slot, char *out, size_t size)
{
    char path[SwanStationPS5_PATH_MAX];
    app_state_path(path, sizeof(path), slot);
    struct stat st;
    out[0] = '\0';
    if (stat(path, &st) != 0)
        return;
    long ago = (long)(time(NULL) - st.st_mtime);
    if (ago < 60)
        str_copy(out, size, tr("Just now"));
    else if (ago < 3600)
        snprintf(out, size, tr("%ld min ago"), ago / 60);
    else if (ago < 86400)
        snprintf(out, size, tr("%ld h ago"), ago / 3600);
    else if (ago < 172800)
        str_copy(out, size, tr("Yesterday"));
    else
        snprintf(out, size, tr("%ld days ago"), ago / 86400);
}

/* The note written on a slot from the phone (<state>.note), re-read every
 * two seconds so a note typed while the menu is open shows up. */
static const char *slot_note(int slot)
{
    static char notes[10][124];
    static uint64_t read_at;
    static const Game *read_for;
    uint64_t now = plat_ticks_us();
    if (read_for != app.game || now - read_at > 2000000)
    {
        read_for = app.game;
        read_at = now;
        for (int s = 0; s < 10; ++s)
        {
            char path[SwanStationPS5_PATH_MAX + 8];
            app_state_path(path, sizeof(path) - 8, s);
            strcat(path, ".note");
            notes[s][0] = '\0';
            FILE *f = fopen(path, "rb");
            if (f)
            {
                size_t n = fread(notes[s], 1, sizeof(notes[s]) - 1, f);
                notes[s][n] = '\0';
                fclose(f);
            }
        }
    }
    return notes[slot];
}

static void draw_slots(float x, float y, float w, bool active)
{
    const int visible = 5;
    const float gap = 14, cw = (w - gap * (visible - 1)) / visible, ch = cw * 0.75f;
    int slot = app.settings.state_slot;
    float target = (float)slot;
    anim_approach(&M.slot_x, target, app.dt, TH_SNAP);
    text_draw(x, y - 40, 22, FONT_BOLD, TH_TEXT_DIM, ALIGN_LEFT, tr("State slots"));
    if (active)
        text_draw(x + w, y - 40, 22, FONT_REGULAR, TH_HINT, ALIGN_RIGHT, tr("Left / Right  Slot"));
    plat_set_clip((int)x - 6, (int)y - 6, (int)w + 12, (int)ch + 12);
    float start = x + w * 0.5f - cw * 0.5f - M.slot_x * (cw + gap);
    for (int s = 0; s < 10; ++s)
    {
        float cx = start + s * (cw + gap);
        if (cx + cw < x - 10 || cx > x + w + 10)
            continue;
        char age[48];
        slot_age(s, age, sizeof(age));
        bool on = s == slot;
        draw_rrect(cx, y, cw, ch, TH_RADIUS_SMALL, on ? TH_ROW_SELECTED : age[0] ? TH_CARD : TH_BG_DEEP);
        PlatTexture *thumb = age[0] ? slot_thumb(s) : NULL;
        if (thumb)
        {
            /* the picture, with the name and age on a dark strip */
            plat_draw_texture(thumb, cx + 4, y + 4, cw - 8, ch - 8, 0xffffffffu, false);
            draw_rect(cx + 4, y + ch - 52, cw - 8, 48, 0xc0000000u);
        }
        if (on)
            draw_rrect_outline(cx, y, cw, ch, TH_RADIUS_SMALL, 3, active ? TH_FOCUS : TH_SWITCH_OFF);
        char name[32];
        snprintf(name, sizeof(name), tr("Slot %d"), s);
        const char *note = age[0] ? slot_note(s) : "";
        if (thumb)
        {
            /* a note replaces the slot's name: "Before the boss" */
            text_draw_fit(cx + 14, y + ch - 46, 18, FONT_BOLD, TH_TEXT, ALIGN_LEFT, cw - 24, note[0] ? note : name);
            text_draw_fit(cx + 14, y + ch - 24, 16, FONT_REGULAR, TH_TEXT_SOFT, ALIGN_LEFT, cw - 24, age);
        }
        else
        {
            text_draw_fit(cx + 16, y + 14, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, cw - 64, note[0] ? note : name);
            text_draw_fit(cx + 16, y + ch - 40, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, cw - 24,
                          age[0] ? age : tr("Empty"));
            icon_draw(age[0] ? ICON_DEVICE_FLOPPY : ICON_X, cx + cw - 44, y + 12, 28,
                      age[0] ? TH_FOCUS : TH_SWITCH_OFF);
        }
    }
    plat_set_clip(0, 0, 0, 0);
}

void menu_screen(uint32_t pressed)
{
    /* ------------------------------------------------ input */
    int before = M.cursor;
    int step = (pressed & BIT(BTN_DOWN)) ? 1 : (pressed & BIT(BTN_UP)) ? -1 : 0;
    if (step)
    {
        do
            M.cursor = (M.cursor + step + MI_COUNT) % MI_COUNT;
        while (!item_shown(M.cursor));
    }
    if (M.cursor != before)
        sfx_play(SFX_CLICK);
    bool on_slots = M.cursor == MI_SAVE || M.cursor == MI_LOAD;
    bool left = pressed & BIT(BTN_LEFT), right = pressed & BIT(BTN_RIGHT);
    if (on_slots && (left || right))
    {
        app.settings.state_slot = (app.settings.state_slot + (right ? 1 : 9)) % 10;
        app.global.state_slot = app.settings.state_slot;
        sfx_play(SFX_CLICK);
    }
    int auto_slots[AUTO_SLOTS];
    long auto_ages[AUTO_SLOTS];
    int autos = play_auto_list(auto_slots, auto_ages);
    if (M.auto_choice >= autos)
        M.auto_choice = 0;
    if (M.cursor == MI_AUTO && autos > 1 && (left || right))
    {
        M.auto_choice = (M.auto_choice + (right ? 1 : autos - 1)) % autos;
        sfx_play(SFX_CLICK);
    }
    int discs = host_disc_count();
    if (M.cursor == MI_DISC && discs > 1 && (left || right))
    {
        M.disc_choice = (M.disc_choice + (right ? 1 : discs - 1)) % discs;
        sfx_play(SFX_CLICK);
    }
    if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_MENU)))
    {
        sfx_play(SFX_BACK);
        app.screen = SCREEN_GAME;
        return;
    }
    if (pressed & BIT(BTN_CROSS))
    {
        char msg[96], st[SwanStationPS5_PATH_MAX];
        app_state_path(st, sizeof(st), app.settings.state_slot);
        switch (M.cursor)
        {
        case MI_RESUME:
            app.screen = SCREEN_GAME;
            return;
        case MI_SAVE:
            make_dirs(app.paths.states);
            if (host_save_state(st))
            {
                play_save_thumb(st);
                snprintf(msg, sizeof(msg), tr("Saved to slot %d"), app.settings.state_slot);
                app.screen = SCREEN_GAME;
            }
            else
                snprintf(msg, sizeof(msg), tr("Couldn't save to slot %d"), app.settings.state_slot);
            app_toast(msg);
            sfx_play(SFX_SELECT);
            return;
        case MI_LOAD:
            if (ra_hardcore())
                str_copy(msg, sizeof(msg), tr("Not allowed in hardcore mode"));
            else if (host_load_state(st))
            {
                snprintf(msg, sizeof(msg), tr("Loaded slot %d"), app.settings.state_slot);
                app.screen = SCREEN_GAME;
            }
            else
                snprintf(msg, sizeof(msg), tr("Slot %d is empty"), app.settings.state_slot);
            app_toast(msg);
            sfx_play(SFX_SELECT);
            return;
        case MI_AUTO:
        {
            char path[SwanStationPS5_PATH_MAX];
            if (ra_hardcore())
                str_copy(msg, sizeof(msg), tr("Not allowed in hardcore mode"));
            else if (autos > 0)
            {
                play_auto_path(auto_slots[M.auto_choice], path, sizeof(path));
                if (host_load_state(path))
                {
                    str_copy(msg, sizeof(msg), tr("Auto-save loaded"));
                    app.screen = SCREEN_GAME;
                }
                else
                    str_copy(msg, sizeof(msg), tr("Could not load the auto-save"));
            }
            else
                msg[0] = '\0';
            if (msg[0])
                app_toast(msg);
            sfx_play(SFX_SELECT);
            return;
        }
        case MI_GUIDE:
            sfx_play(SFX_SELECT);
            guide_open();
            return;
        case MI_DISC:
            if (host_disc_select(M.disc_choice))
            {
                snprintf(msg, sizeof(msg), tr("Disc %d inserted"), M.disc_choice + 1);
                app.screen = SCREEN_GAME;
            }
            else
                str_copy(msg, sizeof(msg), tr("Disc change failed"));
            app_toast(msg);
            return;
        case MI_CHEATS:
            if (ra_hardcore())
            {
                app_toast("Cheats are off in hardcore mode");
                return;
            }
            M.cheat_cursor = 0;
            M.cheat_scroll = 0;
            M.cheat_y = 0;
            app.screen = SCREEN_CHEATS;
            sfx_play(SFX_SELECT);
            return;
        case MI_ACHIEVEMENTS:
            sfx_play(SFX_SELECT);
            achievements_open();
            return;
        case MI_MANUAL:
            sfx_play(SFX_SELECT);
            manual_open();
            return;
        case MI_SETTINGS:
            sfx_play(SFX_SELECT);
            app_open_settings(SCREEN_MENU);
            return;
        case MI_RESET:
            host_reset();
            ra_reset();
            app_toast("Console reset");
            app.screen = SCREEN_GAME;
            return;
        case MI_QUIT:
            app_save_settings();
            sfx_play(SFX_BACK);
            app_stop_game();
            shelf_select_game(app.global.last_game);
            return;
        default:
            break;
        }
    }

    /* ------------------------------------------------ draw */
    M.open_t = fminf(M.open_t + app.dt * 7.0f, 1.0f);
    float ease = 1.0f - (1.0f - M.open_t) * (1.0f - M.open_t);
    app_draw_game((uint8_t)(255 - 150 * ease));

    const float pw = 620;
    float px = -pw * (1.0f - ease);
    draw_rect(px, 0, pw, plat_height(), TH_BG_A(0xf0));
    draw_rect(px + pw, 0, 3, plat_height(), argb_alpha(TH_DIVIDER, ease));

    /* the game: cover, title, serial, achievements */
    const Game *g = app.game;
    PlatTexture *cover = g ? covers_get(app.game_index) : NULL;
    float cx = px + 48, cy = 56;
    if (cover)
    {
        int tw = 1, th = 1;
        plat_texture_size(cover, &tw, &th);
        float h = 150, w = h * tw / th;
        plat_draw_texture(cover, cx, cy, w, h, 0xffffffffu, true);
        cx += w + 28;
    }
    if (g)
    {
        text_draw_fit(cx, cy + 20, 32, FONT_BOLD, TH_TEXT, ALIGN_LEFT, px + pw - cx - 40, shelf_game_title(g));
        /* the serial and the storage, as pills like the shelf's (the second wraps if it does not fit) */
        char folder[64];
        shelf_folder_tag(g, folder, sizeof(folder));
        const char *tags[2] = {g->serial[0] ? g->serial : tr("No serial"), folder};
        const float limit = px + pw - 40, pill_h = 38;
        float tx = cx, ty = cy + 64;
        for (int i = 0; i < 2; ++i)
        {
            if (!tags[i][0])
                continue;
            float tw = text_width(20, FONT_REGULAR, tags[i]) + pill_h * 0.9f;
            if (tx > cx && tx + tw > limit)
            {
                tx = cx;
                ty += pill_h + 8;
            }
            tx += draw_pill(tx, ty, pill_h, 20, TH_PILL, TH_TEXT_SOFT, tags[i]) + 10;
        }
        int unlocked, total;
        if (ra_game_progress(&unlocked, &total))
        {
            char a[64];
            snprintf(a, sizeof(a), tr("%d of %d achievements"), unlocked, total);
            icon_draw(ICON_TROPHY, cx, ty + pill_h + 14, 26, TH_GOLD);
            text_draw(cx + 36, ty + pill_h + 14, 22, FONT_REGULAR, TH_TEXT_SOFT, ALIGN_LEFT, a);
        }
    }

    static const struct
    {
        const char *name;
        int icon;
    } ITEMS[MI_COUNT] = {
        {"Resume", ICON_PLAYER_PLAY},    {"Save state", ICON_DEVICE_FLOPPY},
        {"Load state", ICON_HISTORY},    {"Auto-saves", ICON_HISTORY},
        {"Disc", ICON_DISC},
        {"Cheats", ICON_CODE},           {"Achievements", ICON_TROPHY},
        {"Manual", ICON_BOOKS},          {"Guide", ICON_FILE_TEXT},
        {"Settings", ICON_ADJUSTMENTS},
        {"Reset", ICON_REFRESH},         {"Quit to shelf", ICON_DOOR_EXIT},
    };
    const float row_h = 72, top = 270, x = px + 32, w = pw - 64;
    float y = top, sel_target = top;
    for (int i = 0; i < MI_COUNT; ++i)
    {
        if (!item_shown(i))
            continue;
        if (i == M.cursor)
            sel_target = y;
        y += row_h;
    }
    if (M.sel_y == 0)
        M.sel_y = sel_target;
    anim_approach(&M.sel_y, sel_target, app.dt, TH_SNAP);
    draw_rrect(x, M.sel_y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
    draw_rrect_outline(x, M.sel_y, w, row_h - 8, TH_RADIUS_SMALL, 3, TH_FOCUS);
    y = top;
    for (int i = 0; i < MI_COUNT; ++i)
    {
        if (!item_shown(i))
            continue;
        uint32_t c = i == MI_QUIT ? TH_DANGER : TH_TEXT;
        icon_draw(ITEMS[i].icon, x + 22, y + 16, 32, i == MI_QUIT ? TH_DANGER : TH_FOCUS);
        text_draw(x + 74, y + 17, 28, FONT_REGULAR, c, ALIGN_LEFT, tr(ITEMS[i].name));
        char value[64] = "";
        if (i == MI_AUTO && autos > 0)
        {
            long ago = auto_ages[M.auto_choice];
            if (ago < 60)
                str_copy(value, sizeof(value), tr("Just now"));
            else if (ago < 3600)
                snprintf(value, sizeof(value), tr("%ld min ago"), ago / 60);
            else if (ago < 86400)
                snprintf(value, sizeof(value), tr("%ld h ago"), ago / 3600);
            else
                snprintf(value, sizeof(value), tr("%ld days ago"), ago / 86400);
            if (autos > 1)
            {
                draw_choice(x + w - 16, y + 12, 42, 22, TH_PILL, TH_TEXT_SOFT, value);
                value[0] = '\0';
            }
        }
        else if (i == MI_DISC)
        {
            snprintf(value, sizeof(value), tr("%d of %d"), M.disc_choice + 1, discs);
            draw_choice(x + w - 16, y + 12, 42, 22, TH_PILL, TH_TEXT_SOFT, value);
            value[0] = '\0';
        }
        else if (i == MI_CHEATS)
        {
            int on = 0;
            for (int k = 0; k < app.cheats.count; ++k)
                on += app.cheats.items[k].enabled;
            if (ra_hardcore())
                str_copy(value, sizeof(value), tr("hardcore"));
            else if (app.cheats.count)
                snprintf(value, sizeof(value), tr("%d on"), on);
        }
        else if (i == MI_LOAD && ra_hardcore())
            str_copy(value, sizeof(value), tr("hardcore"));
        else if (i == MI_ACHIEVEMENTS)
        {
            int unlocked, total;
            if (ra_game_progress(&unlocked, &total))
                snprintf(value, sizeof(value), "%d / %d", unlocked, total);
        }
        if (value[0])
            text_draw(x + w - 24, y + 19, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, value);
        y += row_h;
    }

    /* the time, how long you've played, and the controllers' batteries */
    {
        char clock[32], played[64] = "";
        plat_clock(clock, sizeof(clock));
        int minutes = (int)(app.play_seconds / 60.0);
        if (minutes < 1)
            str_copy(played, sizeof(played), tr("Just started"));
        else if (minutes < 60)
            snprintf(played, sizeof(played), tr("Playing for %d min"), minutes);
        else
            snprintf(played, sizeof(played), tr("Playing for %d h %02d"), minutes / 60, minutes % 60);
        char batteries[96] = "";
        for (int k = 0; k < SwanStationPS5_MAX_PADS; ++k)
        {
            int level = app.pads[k].connected ? plat_pad_battery(k) : -1;
            if (level >= 0)
                snprintf(batteries + strlen(batteries), sizeof(batteries) - strlen(batteries), "%sP%d %d%%",
                         batteries[0] ? "   " : "", k + 1, level);
        }
        float cw = fmaxf(text_width(56, FONT_BOLD, clock), text_width(22, FONT_REGULAR, played)) + 64;
        if (batteries[0])
            cw = fmaxf(cw, text_width(22, FONT_REGULAR, batteries) + 64);
        float ch = batteries[0] ? 160 : 128, rx = plat_width() - TH_MARGIN - cw;
        draw_rrect(rx, 48, cw, ch, TH_RADIUS, argb_alpha(TH_BG_A(0xe0), ease));
        text_draw(rx + cw - 32, 60, 56, FONT_BOLD, argb_alpha(TH_TEXT, ease), ALIGN_RIGHT, clock);
        text_draw(rx + cw - 32, 132, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, ease), ALIGN_RIGHT, played);
        if (batteries[0])
            text_draw(rx + cw - 32, 164, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, ease), ALIGN_RIGHT, batteries);
    }

    /* save slots, along the bottom right */
    float sx = px + pw + 80, sw = plat_width() - sx - TH_MARGIN;
    if (sw > 400)
        draw_slots(sx, 780, sw, on_slots);

    static const int glyphs[] = {GLYPH_CROSS, GLYPH_CIRCLE};
    static const char *const labels[] = {"Select", "Resume"};
    app_draw_hints(glyphs, labels, 2, NULL);
    app_draw_toast();
}

/* ---------------------------------------------------------------- cheats */

void cheats_screen(uint32_t pressed)
{
    CheatList *cl = &app.cheats;
    const int rows = 10;
    const float row_h = 70, top = 180, x = 300, w = plat_width() - 600.0f;
    if (cl->count > 0)
    {
        int before = M.cheat_cursor;
        if (pressed & BIT(BTN_UP))
            M.cheat_cursor = (M.cheat_cursor + cl->count - 1) % cl->count;
        if (pressed & BIT(BTN_DOWN))
            M.cheat_cursor = (M.cheat_cursor + 1) % cl->count;
        if (pressed & BIT(BTN_L1))
            M.cheat_cursor = M.cheat_cursor > rows ? M.cheat_cursor - rows : 0;
        if (pressed & BIT(BTN_R1))
            M.cheat_cursor = M.cheat_cursor + rows < cl->count ? M.cheat_cursor + rows : cl->count - 1;
        if (M.cheat_cursor != before)
            sfx_play(SFX_CLICK);
        if (pressed & BIT(BTN_CROSS))
        {
            cl->items[M.cheat_cursor].enabled = !cl->items[M.cheat_cursor].enabled;
            cheats_apply(cl);
            cheats_save_selection(cl);
            sfx_play(SFX_SELECT);
        }
        if (pressed & BIT(BTN_SQUARE))
        {
            for (int i = 0; i < cl->count; ++i)
                cl->items[i].enabled = false;
            cheats_apply(cl);
            cheats_save_selection(cl);
            app_toast("All cheats off");
        }
    }
    if (pressed & BIT(BTN_TRIANGLE))
    {
        sfx_play(SFX_SELECT);
        cheat_search_open();
        return;
    }
    if (pressed & BIT(BTN_CIRCLE))
    {
        sfx_play(SFX_BACK);
        app.screen = SCREEN_MENU;
        return;
    }

    app_draw_game(40);
    draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);
    text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Cheats"));
    const char *src = strrchr(cl->source, '/');
    if (cl->count)
        text_draw_fit(TH_MARGIN + 2, 100, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, 1400,
                      src ? src + 1 : cl->source);
    if (cl->count == 0)
    {
        float bw = 1300, bx = (plat_width() - bw) * 0.5f, by = 360;
        draw_rrect(bx, by, bw, 260, TH_RADIUS, TH_CARD);
        icon_draw(ICON_CODE, plat_width() * 0.5f - 32, by + 32, 64, TH_FOCUS);
        text_draw(plat_width() * 0.5f, by + 112, 34, FONT_BOLD, TH_TEXT, ALIGN_CENTER,
                  tr("No cheats for this game"));
        char where[SwanStationPS5_PATH_MAX + 64];
        snprintf(where, sizeof(where), tr("Put .cht files in %s, or one next to the game."), app.paths.cheats);
        text_draw_fit(plat_width() * 0.5f, by + 168, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, bw - 80, where);
        text_draw(plat_width() * 0.5f, by + 206, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                  tr("tools/SwanStationPS5_sync.py cheats installs the libretro cheat library."));
    }
    else
    {
        /* a whole first row: animating towards the animated value stopped the
         * list a fraction short of the last row */
        static int top_row;
        top_row = list_top_row(top_row, M.cheat_cursor, rows, cl->count);
        anim_approach(&M.cheat_scroll, (float)top_row, app.dt, TH_SNAP);
        draw_rrect(x - 16, top - 16, w + 32, rows * row_h + 24, TH_RADIUS, TH_CARD);
        plat_set_clip((int)x - 8, (int)top - 8, (int)w + 16, (int)(rows * row_h) + 8);
        float target = top + (M.cheat_cursor - M.cheat_scroll) * row_h;
        if (M.cheat_y == 0)
            M.cheat_y = target;
        anim_approach(&M.cheat_y, target, app.dt, TH_SNAP * 1.5f);
        draw_rrect(x, M.cheat_y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
        draw_rrect_outline(x, M.cheat_y, w, row_h - 8, TH_RADIUS_SMALL, 3, TH_FOCUS);
        int first = (int)floorf(M.cheat_scroll);
        for (int i = first; i < cl->count && i <= first + rows; ++i)
        {
            const Cheat *c = &cl->items[i];
            float y = top + (i - M.cheat_scroll) * row_h;
            draw_switch(x + 24, y + 15, 34, c->enabled ? 1.0f : 0.0f);
            float tag = 0;
            if (c->patch)
            {
                /* widescreen, 60 fps, fixes: not a cheat */
                tag = text_width(20, FONT_BOLD, tr("Patch")) + 32;
                draw_rrect(x + w - 24 - tag, y + 15, tag, 34, 17, TH_PILL);
                text_draw(x + w - 24 - tag * 0.5f, y + 20, 20, FONT_BOLD, TH_FOCUS, ALIGN_CENTER, tr("Patch"));
            }
            text_draw_fit(x + 130, y + 17, 26, FONT_REGULAR, TH_TEXT, ALIGN_LEFT, w - 180 - tag, c->desc);
        }
        plat_set_clip(0, 0, 0, 0);
    }
    static const int glyphs[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_TRIANGLE, GLYPH_CIRCLE};
    static const char *const labels[] = {"Toggle", "All off", "Find a code", "Back"};
    app_draw_hints(glyphs, labels, 4, NULL);
    if (cl->count)
    {
        const HintCombo right[1] = {{{GLYPH_L1, GLYPH_R1}, 2, '/', "Page"}};
        app_draw_hints_right(right, 1);
    }
    app_draw_toast();
}

/* ---------------------------------------------------------------- achievements */

#define MAX_ACH 400

static struct
{
    RaAchievement list[MAX_ACH];
    int count, cursor;
    float scroll, sel_y;
    PlatTexture *badge[MAX_ACH];
    SDL_atomic_t badge_ready[MAX_ACH]; /* 1 = file on disk, 2 = no picture */
    SDL_Thread *fetcher;
    SDL_atomic_t stop;
} A;

static void badge_path(int i, char *out, size_t size)
{
    char file[48];
    snprintf(file, sizeof(file), "cache/badges/%u_%d.png", A.list[i].id, A.list[i].unlocked ? 1 : 0);
    path_join(out, size, app.paths.root, file);
}

/* downloads missing badge pictures, nearest first, off the main thread */
static int fetch_badges(void *unused)
{
    (void)unused;
    char dir[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "cache/badges");
    make_dirs(dir);
    for (int i = 0; i < A.count && !SDL_AtomicGet(&A.stop); ++i)
    {
        char path[SwanStationPS5_PATH_MAX];
        badge_path(i, path, sizeof(path));
        bool ok = path_exists(path) ||
                  (A.list[i].badge_url[0] && net_download(A.list[i].badge_url, path) == NET_OK);
        SDL_AtomicSet(&A.badge_ready[i], ok ? 1 : 2);
    }
    return 0;
}

static void stop_fetcher(void)
{
    if (A.fetcher)
    {
        SDL_AtomicSet(&A.stop, 1);
        SDL_WaitThread(A.fetcher, NULL);
        A.fetcher = NULL;
    }
    for (int i = 0; i < MAX_ACH; ++i)
    {
        plat_texture_free(A.badge[i]);
        A.badge[i] = NULL;
        SDL_AtomicSet(&A.badge_ready[i], 0);
    }
}

/* The list for the tab: "Achievable" (still locked, the closest first) or
 * "Achieved". view[] holds indices into A.list. */
static int ach_view[MAX_ACH], ach_view_count, ach_tab;

static void build_ach_view(void)
{
    ach_view_count = 0;
    for (int i = 0; i < A.count; ++i)
        if (A.list[i].unlocked == (ach_tab == 1))
            ach_view[ach_view_count++] = i;
    if (ach_tab == 0) /* insertion sort: furthest along first, list order otherwise */
        for (int i = 1; i < ach_view_count; ++i)
        {
            int v = ach_view[i], j = i;
            while (j > 0 && A.list[ach_view[j - 1]].percent < A.list[v].percent)
            {
                ach_view[j] = ach_view[j - 1];
                --j;
            }
            ach_view[j] = v;
        }
    A.cursor = 0;
    A.scroll = 0;
    A.sel_y = 0;
}

void achievements_open(void)
{
    stop_fetcher();
    A.count = ra_list(A.list, MAX_ACH);
    ach_tab = 0;
    build_ach_view();
    A.cursor = 0;
    A.scroll = 0;
    A.sel_y = 0;
    SDL_AtomicSet(&A.stop, 0);
    A.fetcher = A.count ? SDL_CreateThread(fetch_badges, "badges", NULL) : NULL;
    app.screen = SCREEN_ACHIEVEMENTS;
}

void achievements_screen(uint32_t pressed)
{
    const int rows = 7;
    const float row_h = 104, top = 214, x = 260, w = plat_width() - 520.0f;
    if (pressed & (BIT(BTN_L1) | BIT(BTN_R1) | BIT(BTN_LEFT) | BIT(BTN_RIGHT)))
    {
        ach_tab ^= 1;
        build_ach_view();
        sfx_play(SFX_CLICK);
    }
    const int count = ach_view_count;
    if (count)
    {
        int before = A.cursor;
        if (pressed & BIT(BTN_UP))
            A.cursor = (A.cursor + count - 1) % count;
        if (pressed & BIT(BTN_DOWN))
            A.cursor = (A.cursor + 1) % count;
        if (pressed & BIT(BTN_L2))
            A.cursor = A.cursor > rows ? A.cursor - rows : 0;
        if (pressed & BIT(BTN_R2))
            A.cursor = A.cursor + rows < count ? A.cursor + rows : count - 1;
        if (A.cursor != before)
            sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_CIRCLE))
    {
        stop_fetcher();
        sfx_play(SFX_BACK);
        app.screen = SCREEN_MENU;
        return;
    }

    app_draw_game(40);
    draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);
    text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Achievements"));
    int unlocked = 0, points = 0, total_points = 0;
    for (int i = 0; i < A.count; ++i)
    {
        unlocked += A.list[i].unlocked;
        total_points += (int)A.list[i].points;
        points += A.list[i].unlocked ? (int)A.list[i].points : 0;
    }
    char sub[160];
    snprintf(sub, sizeof(sub), tr("%d of %d unlocked  \xc2\xb7  %d of %d points"), unlocked, A.count, points,
             total_points);
    text_draw(TH_MARGIN + 2, 100, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, sub);
    if (A.count)
    {
        float bw = 420, bx = plat_width() - TH_MARGIN - bw;
        draw_rrect(bx, 62, bw, 10, 5, TH_PILL);
        draw_rrect(bx, 62, bw * unlocked / A.count, 10, 5, TH_GOLD);
    }
    /* the two tabs */
    {
        char label[2][64];
        snprintf(label[0], sizeof(label[0]), "%s  %d", tr("Achievable"), A.count - unlocked);
        snprintf(label[1], sizeof(label[1]), "%s  %d", tr("Achieved"), unlocked);
        float tx = x;
        for (int t = 0; t < 2; ++t)
        {
            float tw = text_width(22, FONT_REGULAR, label[t]) + 44;
            bool on = t == ach_tab;
            draw_rrect(tx, 146, tw, 46, 23, on ? TH_TEXT : TH_PILL);
            text_draw(tx + tw * 0.5f, 156, 22, on ? FONT_BOLD : FONT_REGULAR, on ? TH_BG : TH_TEXT_DIM,
                      ALIGN_CENTER, label[t]);
            tx += tw + 12;
        }
    }

    static int top_row;
    top_row = list_top_row(top_row, A.cursor, rows, count);
    anim_approach(&A.scroll, (float)top_row, app.dt, TH_SNAP);
    draw_rrect(x - 16, top - 16, w + 32, rows * row_h + 24, TH_RADIUS, TH_CARD);
    plat_set_clip((int)x - 8, (int)top - 8, (int)w + 16, (int)(rows * row_h) + 8);
    if (count)
    {
        float target = top + (A.cursor - A.scroll) * row_h;
        if (A.sel_y == 0)
            A.sel_y = target;
        anim_approach(&A.sel_y, target, app.dt, TH_SNAP * 1.5f);
        draw_rrect(x, A.sel_y, w, row_h - 8, TH_RADIUS_SMALL, TH_ROW_SELECTED);
        draw_rrect_outline(x, A.sel_y, w, row_h - 8, TH_RADIUS_SMALL, 3, TH_FOCUS);
    }
    int first = (int)floorf(A.scroll);
    for (int k = first; k < count && k <= first + rows; ++k)
    {
        const int i = ach_view[k];
        const RaAchievement *a = &A.list[i];
        float y = top + (k - A.scroll) * row_h;
        /* the badge, once downloaded; a trophy or a lock until then */
        if (!A.badge[i] && SDL_AtomicGet(&A.badge_ready[i]) == 1)
        {
            char path[SwanStationPS5_PATH_MAX];
            badge_path(i, path, sizeof(path));
            int bw, bh, comp;
            unsigned char *px = stbi_load(path, &bw, &bh, &comp, 4);
            if (px)
            {
                A.badge[i] = plat_texture_create(px, bw, bh, true);
                stbi_image_free(px);
            }
            else
                SDL_AtomicSet(&A.badge_ready[i], 2);
        }
        if (A.badge[i])
            plat_draw_texture(A.badge[i], x + 14, y + 10, 64, 64, a->unlocked ? 0xffffffffu : 0xff707070u, false);
        else
        {
            draw_rrect(x + 14, y + 10, 64, 64, 10, a->unlocked ? 0xff2a2410u : TH_PILL);
            icon_draw(a->unlocked ? ICON_TROPHY : ICON_LOCK, x + 26, y + 22, 40,
                      a->unlocked ? TH_GOLD : TH_TEXT_DIM);
        }
        text_draw_fit(x + 100, y + 12, 26, FONT_BOLD, a->unlocked ? TH_TEXT : TH_TEXT_SOFT, ALIGN_LEFT,
                      w - 300, a->title);
        text_draw_fit(x + 100, y + 48, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w - 300, a->description);
        char pts[32];
        snprintf(pts, sizeof(pts), tr("%u points"), a->points);
        text_draw(x + w - 24, y + 14, 22, FONT_REGULAR, a->unlocked ? TH_GOLD : TH_TEXT_DIM, ALIGN_RIGHT, pts);
        if (a->unlocked)
            icon_draw(ICON_CIRCLE_CHECK, x + w - 54, y + 46, 30, TH_GOOD);
        else if (a->progress[0])
        {
            /* counted: 18/80 dragons, as a bar */
            char pc[48];
            snprintf(pc, sizeof(pc), "%s  \xc2\xb7  %d%%", a->progress, (int)(a->percent + 0.5f));
            text_draw(x + w - 24, y + 46, 20, FONT_REGULAR, TH_FOCUS, ALIGN_RIGHT, pc);
            float bx = x + 100, bw = w - 324, k = a->percent < 0 ? 0 : a->percent > 100 ? 1 : a->percent / 100.0f;
            draw_rrect(bx, y + 80, bw, 8, 4, TH_PILL);
            if (k > 0)
                draw_rrect(bx, y + 80, bw * k < 8 ? 8 : bw * k, 8, 4, TH_GOLD);
        }
    }
    plat_set_clip(0, 0, 0, 0);
    if (!A.count)
        text_draw(plat_width() * 0.5f, top + 200, 28, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                  tr("No achievements loaded for this game"));
    else if (!count)
        text_draw(plat_width() * 0.5f, top + 200, 28, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                  tr(ach_tab ? "None unlocked yet" : "All unlocked!"));
    static const int glyphs[] = {GLYPH_CIRCLE};
    static const char *const labels[] = {"Back"};
    app_draw_hints(glyphs, labels, 1, NULL);
    if (A.count)
    {
        const HintCombo right[2] = {{{GLYPH_L1, GLYPH_R1}, 2, '/', "Tab"}, {{GLYPH_L2, GLYPH_R2}, 2, '/', "Page"}};
        app_draw_hints_right(right, 2);
    }
    app_draw_toast();
}
