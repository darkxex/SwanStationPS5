/*
 * SwanStationPS5 - PlayStation X Super 5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Entry point, game start/stop and the emulation screen. The other screens
 * live in src/ui (shelf, settings, in-game menu and cheats).
 * Emulation is SwanStation, linked statically and driven through libretro.
 */
#include "app.h"

#include "config.h"
#include "core/host.h"
#include "bezels.h"
#include "controls.h"
#include "gamedb.h"
#include "profiles.h"
#include "covers.h"
#include "i18n.h"
#include "platform/platform.h"
#include "platform/ps5_crash.h"
#include "platform/vk/vk_probe.h"
#include "play.h"
#include "remote.h"
#include "update.h"
#include "ra/achievements.h"
#include "stats.h"
#include "ui/coverflow.h"
#include "ui/draw.h"
#include "ui/icons.h"
#include "ui/sfx.h"
#include "ui/text.h"
#include "ui/theme.h"

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <time.h>

#define UI_RATE 44100

App app = {.unlock_setting = true, .dt = 1.0f / 60.0f};

static char toast[160];
static uint64_t toast_until;
/* messages that arrive while one shows wait their turn */
static char toast_queue[4][160];
static uint64_t toast_queue_us[4];
static uint64_t toast_total_us = 2500000; /* how long the one on screen stays */
static int toast_queued;
static int covers_style_loaded = -1;
static bool covers_download_loaded;

static void show_next_toast(const char *text, uint64_t length_us)
{
    str_copy(toast, sizeof(toast), text);
    toast_total_us = length_us;
    toast_until = plat_ticks_us() + length_us;
}

/* Every notice ends as a sentence: a full stop is added unless it already ends in punctuation (or a button picture) */
static const char *toast_sentence(const char *text, char *out, size_t size)
{
    size_t n = strlen(text);
    if (n == 0 || n + 2 > size)
        return text;
    char last = text[n - 1];
    if (strchr(".!?:;]", last) || (n >= 3 && !memcmp(text + n - 3, "\xE2\x80\xA6", 3))) /* … */
        return text;
    memcpy(out, text, n);
    out[n] = '.';
    out[n + 1] = '\0';
    return out;
}

void app_toast(const char *message)
{
    app_toast_for(message, 2.5f);
}

void app_toast_for(const char *message, float seconds)
{
    uint64_t length_us = (uint64_t)(seconds * 1e6f);
    SwanStationPS5_log("%s", message);
    char sentence[sizeof(toast)];
    const char *text = toast_sentence(tr(message), sentence, sizeof(sentence));
    if (toast[0] && plat_ticks_us() < toast_until)
    {
        if (!strcmp(toast, text))
            return;
        if (toast_queued < 4)
        {
            toast_queue_us[toast_queued] = length_us;
            str_copy(toast_queue[toast_queued++], sizeof(toast_queue[0]), text);
        }
        return;
    }
    show_next_toast(text, length_us);
}

/* A message's line, with the pictures of the buttons where it says [touchpad], [L3] or [R3]; its width (drawn when asked) */
static float toast_runs(const char *s, float x, float y, float a, bool draw)
{
    static const struct
    {
        const char *tag;
        enum PadGlyph glyph;
    } TAGS[] = {{"[touchpad]", GLYPH_TOUCHPAD}, {"[L3]", GLYPH_L3}, {"[R3]", GLYPH_R3}};
    const float size = 26, gap = 10;
    float start = x;
    while (*s)
    {
        const char *next = NULL;
        int which = -1;
        for (int t = 0; t < 3; ++t)
        {
            const char *at = strstr(s, TAGS[t].tag);
            if (at && (!next || at < next))
                next = at, which = t;
        }
        char run[160];
        size_t n = next ? (size_t)(next - s) : strlen(s);
        n = n < sizeof(run) - 1 ? n : sizeof(run) - 1;
        memcpy(run, s, n);
        run[n] = '\0';
        if (n && draw)
            text_draw(x, y, size, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, run);
        x += text_width(size, FONT_REGULAR, run);
        if (!next)
            break;
        float gw = pad_glyph_width(TAGS[which].glyph, size);
        x += gap;
        if (draw)
            draw_pad_glyph(TAGS[which].glyph, x + gw * 0.5f, y + size * 0.6f, size);
        x += gw + gap;
        s = next + strlen(TAGS[which].tag);
    }
    return x - start;
}

void app_draw_toast(void)
{
    uint64_t now = plat_ticks_us();
    if (toast_queued && (!toast[0] || now > toast_until))
    {
        show_next_toast(toast_queue[0], toast_queue_us[0]);
        memmove(toast_queue[0], toast_queue[1], sizeof(toast_queue[0]) * 3);
        memmove(toast_queue_us, toast_queue_us + 1, sizeof(toast_queue_us[0]) * 3);
        --toast_queued;
    }
    if (!toast[0] || now > toast_until)
        return;
    /* slide up in the first 150 ms, fade out in the last 300 ms */
    float shown = (float)(toast_total_us - (toast_until - now)) / 1e6f;
    float left = (float)(toast_until - now) / 1e6f;
    float a = left < 0.3f ? left / 0.3f : 1.0f;
    float rise = shown < 0.15f ? (1.0f - shown / 0.15f) * 24.0f : 0.0f;
    float w = toast_runs(toast, 0, 0, 0, false) + 96, h = 64;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - 190 + rise;
    draw_rrect(x, y, w, h, h * 0.5f, argb_alpha(TH_PILL_A(0xf0), a));
    icon_draw(ICON_INFO_CIRCLE, x + 22, y + 18, 28, argb_alpha(TH_FOCUS, a));
    toast_runs(toast, x + 62, y + 17, a, true);
}

/* ---------------------------------------------------------------- message box */

static char box_text[480];
static bool box_open;
static float box_t; /* 0..1, the fade in */

void app_message_box(const char *message)
{
    str_copy(box_text, sizeof(box_text), tr(message));
    box_open = true;
    box_t = 0.0f;
}

/* X closes it; true while it is up (the screens below get no buttons then) */
static bool message_box_input(uint32_t *pressed)
{
    if (!box_open)
        return false;
    if (*pressed & BIT(BTN_CROSS))
    {
        box_open = false;
        sfx_play(SFX_BACK);
    }
    *pressed = 0;
    return true;
}

static void message_box_draw(void)
{
    if (!box_open)
        return;
    box_t = fminf(box_t + app.dt * 8.0f, 1.0f);
    const float side = 640, pad = 48, size = 28;
    float x = (plat_width() - side) * 0.5f, y = (plat_height() - side) * 0.5f + (1.0f - box_t) * 24.0f;
    draw_rrect(x, y, side, side, TH_RADIUS, argb_alpha(TH_PILL_A(0xf0), box_t));
    icon_draw(ICON_INFO_CIRCLE, x + (side - 56) * 0.5f, y + pad, 56, argb_alpha(TH_FOCUS, box_t));
    /* the message, word-wrapped and centred */
    char line[256];
    float yy = y + pad + 56 + 28;
    const char *m = box_text;
    while (*m && yy < y + side - 110)
    {
        size_t best = 0, i = 0;
        for (;;)
        {
            size_t j = i;
            while (m[j] && m[j] != ' ')
                ++j;
            size_t n = j < sizeof(line) - 1 ? j : sizeof(line) - 1;
            memcpy(line, m, n);
            line[n] = '\0';
            if (best > 0 && text_width(size, FONT_REGULAR, line) > side - 2 * pad)
                break;
            best = n;
            if (!m[j])
                break;
            i = j + 1;
        }
        if (best == 0)
            best = strlen(m) < sizeof(line) - 1 ? strlen(m) : sizeof(line) - 1;
        memcpy(line, m, best);
        line[best] = '\0';
        text_draw(x + side * 0.5f, yy, size, FONT_REGULAR, argb_alpha(TH_TEXT, box_t), ALIGN_CENTER, line);
        yy += size * 1.45f;
        m += best;
        while (*m == ' ')
            ++m;
    }
    /* "(X) Close" at the bottom */
    const char *close = tr("Close");
    const float hs = 26;
    float gw = pad_glyph_width(GLYPH_CROSS, hs);
    float hw = gw + hs * 0.35f + text_width(hs, FONT_REGULAR, close);
    float hx = x + (side - hw) * 0.5f, hy = y + side - pad - hs;
    draw_pad_glyph(GLYPH_CROSS, hx + gw * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw + hs * 0.35f, hy, hs, FONT_REGULAR, argb_alpha(TH_TEXT, box_t), ALIGN_LEFT, close);
}

void app_draw_hints(const int *glyphs, const char *const *labels, int count, const char *right)
{
    draw_rect(TH_MARGIN, TH_HINT_Y - 22, plat_width() - 2 * TH_MARGIN, 2, TH_DIVIDER);
    float x = TH_MARGIN;
    for (int i = 0; i < count; ++i)
        x += draw_hint(x, TH_HINT_Y, (enum PadGlyph)glyphs[i], labels[i], 26, TH_HINT);
    if (right)
        text_draw(plat_width() - TH_MARGIN, TH_HINT_Y, 26, FONT_REGULAR, TH_HINT, ALIGN_RIGHT,
                  tr(right));
}

void app_draw_hints_right(const HintCombo *items, int count)
{
    const float size = 26, gap = size * 1.4f;
    float total = 0;
    for (int i = 0; i < count; ++i)
        total += hint_combo_width(items[i].glyphs, items[i].count, items[i].label, size) +
                 (i < count - 1 ? gap : 0);
    float x = plat_width() - TH_MARGIN - total;
    for (int i = 0; i < count; ++i)
    {
        draw_hint_combo(x, TH_HINT_Y, items[i].glyphs, items[i].count, items[i].sep,
                        items[i].label, size, TH_HINT);
        x += hint_combo_width(items[i].glyphs, items[i].count, items[i].label, size) + gap;
    }
}

/* ---------------------------------------------------------------- input edges */

/* Button presses for menus, with auto-repeat on directions and shoulders. */
static uint32_t nav_pressed(const PadState *pads)
{
    static uint32_t previous;
    static uint64_t repeat_at;
    static int repeats;
    uint32_t held = 0;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        held |= pads[i].buttons;
        if (pads[i].ly < -20000)
            held |= BIT(BTN_UP);
        if (pads[i].ly > 20000)
            held |= BIT(BTN_DOWN);
        if (pads[i].lx < -20000)
            held |= BIT(BTN_LEFT);
        if (pads[i].lx > 20000)
            held |= BIT(BTN_RIGHT);
    }
    uint32_t pressed = held & ~previous;
    const uint32_t dirs = BIT(BTN_UP) | BIT(BTN_DOWN) | BIT(BTN_LEFT) | BIT(BTN_RIGHT) |
                          BIT(BTN_L1) | BIT(BTN_R1) | BIT(BTN_L2) | BIT(BTN_R2);
    uint64_t now = plat_ticks_us();
    if (pressed & dirs)
    {
        repeat_at = now + 260000;
        repeats = 0;
    }
    else if ((held & dirs) && now >= repeat_at)
    {
        pressed |= held & dirs;
        /* speeds up the longer it is held */
        repeat_at = now + (repeats < 6 ? 70000 : 40000);
        ++repeats;
    }
    previous = held;
    return pressed;
}

/* ---------------------------------------------------------------- helpers */

void app_state_path(char *out, size_t size, int slot)
{
    char file[96];
    snprintf(file, sizeof(file), "%.80s.state%d", app.game ? app.game->id : "game", slot);
    path_join(out, size, app.paths.states, file);
}

void app_describe_bios(char *out, size_t size)
{
    DIR *d = opendir(app.paths.bios);
    out[0] = '\0';
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)))
            if (str_icmp(path_ext(e->d_name), "bin") == 0)
            {
                snprintf(out, size, tr("Real BIOS found: %.60s"), e->d_name);
                break;
            }
        closedir(d);
    }
    if (!out[0])
        str_copy(out, size, tr("No BIOS file in bios/, using OpenBios"));
}

void app_restart_covers(void)
{
    covers_start(&app.library, &app.paths, &app.global);
    covers_style_loaded = app.global.cover_style;
    covers_download_loaded = app.global.cover_download;
}

void app_rescan(void)
{
    /* The games folder, then on each drive both <drive>/<name> and <drive>/<name>/games, for our
     * folder name and for the PSXS5 one. */
    static const char *const drives[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/mnt/ext1"};
    static const char *const names[] = {"SwanStationPS5", "PSXS5"};
    static char drive_roots[2 * 2 * 4][SwanStationPS5_PATH_MAX];
    static char usb_roots[2][SwanStationPS5_PATH_MAX];
    const char *roots[2 + 2 * 2 * 4 + 2];
    int root_count = 0;
    roots[root_count++] = app.paths.games;
    roots[root_count++] = "/data/PSXS5/games";
    for (int d = 0; d < 4; ++d)
        for (int n = 0; n < 2; ++n)
        {
            char *plain = drive_roots[root_count - 2], *games = drive_roots[root_count - 1];
            snprintf(plain, SwanStationPS5_PATH_MAX, "%s/%s", drives[d], names[n]);
            snprintf(games, SwanStationPS5_PATH_MAX, "%s/%s/games", drives[d], names[n]);
            roots[root_count++] = plain;
            roots[root_count++] = games;
        }
    /* Settings > System > SwanStationPS5 > Games folder on USB drives: that name on both drives at once, as the drive's
     * number changes with the ones plugged in */
    if (app.global.usb_folder[0])
        for (int u = 0; u < 2; ++u)
        {
            snprintf(usb_roots[u], SwanStationPS5_PATH_MAX, "/mnt/usb%d/%s", u, app.global.usb_folder);
            roots[root_count++] = usb_roots[u];
        }
    char index[SwanStationPS5_PATH_MAX];
    path_join(index, sizeof(index), app.paths.root, "library.txt");
    if (app.sandboxed)
    {
        /* Can't list folders: the sync tool's index is the library. */
        if (!library_load_index(&app.library, index))
            app.library.count = 0;
    }
    else
    {
        library_scan(&app.library, roots, root_count);
        if (app.library.count == 0)
            library_load_index(&app.library, index);
    }
    shelf_library_changed();
    app_restart_covers();
}

const Paths *app_paths(void)
{
    return &app.paths;
}

void app_game_config_path(char *out, size_t size, const Game *g)
{
    char dir[SwanStationPS5_PATH_MAX], file[96];
    path_join(dir, sizeof(dir), app.paths.user, "game-settings");
    snprintf(file, sizeof(file), "%.80s.ini", g ? g->id : "none");
    path_join(out, size, dir, file);
}

void app_save_settings(void)
{
    config_save(&app.global, app.paths.config);
    if (app.game && app.game_has_own)
    {
        char path[SwanStationPS5_PATH_MAX], dir[SwanStationPS5_PATH_MAX];
        path_join(dir, sizeof(dir), app.paths.user, "game-settings");
        make_dirs(dir);
        app_game_config_path(path, sizeof(path), app.game);
        config_save(&app.settings, path);
    }
    if (app.global.cover_style != covers_style_loaded ||
        app.global.cover_download != covers_download_loaded)
        app_restart_covers();
}

/* ---------------------------------------------------------------- the update question */

/* After an update the first screen asks whether to start from the new version's factory settings: X does, Circle
 * keeps the settings as they are (the answer is saved, so it is asked once). */
static bool question_open;
static float question_t;

static void reset_to_factory(void)
{
    Settings d;
    config_defaults(&d);
    d.last_game = app.global.last_game; /* only where the shelf is stays; the rest, language included, is the factory's */
    app.global = d;
    app.settings = d;
    i18n_set(d.language);
    theme_apply(d.theme);
    sfx_configure(d.ui_sound, (d.ui_volume + 1) * 25);
    remote_update(d.remote);
    plat_set_gpu_ui(d.gpu_ui);
    if (d.gpu_ui)
        plat_set_output(d.output_res);
    app_save_settings();
    app_rescan(); /* the USB folder is back to its own */
    app_toast("Factory settings restored");
}

static void reset_question_open(void)
{
    question_open = true;
    question_t = 0.0f;
}

/* true while it is up: the screens below get no buttons */
static bool question_input(uint32_t *pressed)
{
    if (!question_open)
        return false;
    if (*pressed & BIT(BTN_CROSS))
    {
        question_open = false;
        sfx_play(SFX_SELECT);
        reset_to_factory();
    }
    else if (*pressed & BIT(BTN_CIRCLE))
    {
        question_open = false;
        sfx_play(SFX_BACK);
        app_save_settings(); /* the settings stay; the file now carries this version */
    }
    *pressed = 0;
    return true;
}

static void question_draw(void)
{
    if (!question_open)
        return;
    question_t = fminf(question_t + app.dt * 8.0f, 1.0f);
    const float w = 940, h = 560, pad = 56, size = 30;
    float x = (plat_width() - w) * 0.5f, y = (plat_height() - h) * 0.5f + (1.0f - question_t) * 24.0f;
    draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xa0000000u, question_t));
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_PILL_A(0xf8), question_t));
    icon_draw(ICON_REFRESH, x + (w - 64) * 0.5f, y + pad, 64, argb_alpha(TH_FOCUS, question_t));
    text_draw_fit(x + w * 0.5f, y + pad + 64 + 18, 38, FONT_BOLD, argb_alpha(TH_TEXT, question_t), ALIGN_CENTER, w - 2 * pad,
                  tr("SwanStationPS5 was updated to the latest version!"));
    /* the question, word-wrapped and centred */
    char line[256];
    float yy = y + pad + 64 + 18 + 38 + 26;
    const char *m = tr("Resetting to the factory settings is recommended because of the new improvements. Do you want to do it?");
    while (*m && yy < y + h - 130)
    {
        size_t best = 0, i = 0;
        for (;;)
        {
            size_t j = i;
            while (m[j] && m[j] != ' ')
                ++j;
            size_t n = j < sizeof(line) - 1 ? j : sizeof(line) - 1;
            memcpy(line, m, n);
            line[n] = '\0';
            if (best > 0 && text_width(size, FONT_REGULAR, line) > w - 2 * pad)
                break;
            best = n;
            if (!m[j])
                break;
            i = j + 1;
        }
        if (best == 0)
            best = strlen(m) < sizeof(line) - 1 ? strlen(m) : sizeof(line) - 1;
        memcpy(line, m, best);
        line[best] = '\0';
        text_draw(x + w * 0.5f, yy, size, FONT_REGULAR, argb_alpha(TH_TEXT, question_t), ALIGN_CENTER, line);
        yy += size * 1.45f;
        m += best;
        while (*m == ' ')
            ++m;
    }
    /* the two answers, each with its button: (X) Reset to factory settings   (O) Keep my settings */
    const float hs = 28, gap = hs * 0.4f;
    const char *yes = tr("Reset to factory settings"), *no = tr("Keep my settings");
    const float gw1 = pad_glyph_width(GLYPH_CROSS, hs), gw2 = pad_glyph_width(GLYPH_CIRCLE, hs);
    const float w1 = gw1 + gap + text_width(hs, FONT_BOLD, yes), w2 = gw2 + gap + text_width(hs, FONT_REGULAR, no);
    const float space = 56, hy = y + h - pad - hs, total = w1 + space + w2;
    float hx = x + (w - total) * 0.5f;
    draw_pad_glyph(GLYPH_CROSS, hx + gw1 * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw1 + gap, hy, hs, FONT_BOLD, argb_alpha(TH_FOCUS, question_t), ALIGN_LEFT, yes);
    hx += w1 + space;
    draw_pad_glyph(GLYPH_CIRCLE, hx + gw2 * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw2 + gap, hy, hs, FONT_REGULAR, argb_alpha(TH_TEXT, question_t), ALIGN_LEFT, no);
}

/* ---------------------------------------------------------------- achievements banner */

/* Top right, one message at a time: slides in, holds, fades. */
/* A small card bottom right while a counted achievement moves. */
static void draw_tracker(void)
{
    char title[96], progress[24];
    float percent;
    if (!ra_tracker(title, sizeof(title), progress, sizeof(progress), &percent))
        return;
    const float w = 440, h = 92, x = plat_width() - w - 48, y = plat_height() - h - 48;
    draw_rrect(x, y, w, h, TH_RADIUS, TH_CARD_A(0xe8));
    icon_draw(ICON_TROPHY, x + 18, y + 18, 32, TH_GOLD);
    text_draw_fit(x + 62, y + 16, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, w - 180, title);
    char right[48];
    snprintf(right, sizeof(right), "%s  %d%%", progress, (int)(percent + 0.5f));
    text_draw(x + w - 20, y + 18, 20, FONT_REGULAR, TH_FOCUS, ALIGN_RIGHT, right);
    float bw = w - 40, k = percent < 0 ? 0 : percent > 100 ? 1 : percent / 100.0f;
    draw_rrect(x + 20, y + 62, bw, 10, 5, TH_PILL);
    if (k > 0)
        draw_rrect(x + 20, y + 62, bw * k < 10 ? 10 : bw * k, 10, 5, TH_GOLD);
}

static void draw_achievement(void)
{
    if (app.screen == SCREEN_GAME && app.global.ra_tracker)
        draw_tracker();
    static char title[96], detail[192];
    static uint64_t shown_at;
    const uint64_t length = 4500000;
    static bool showing;
    uint64_t now = plat_ticks_us();
    if (!showing || now > shown_at + length)
    {
        showing = ra_next_message(title, sizeof(title), detail, sizeof(detail));
        if (!showing)
            return;
        /* pop-ups off: messages that arrive during play are dropped, not shown */
        if (!app.global.ra_popups && app.screen == SCREEN_GAME)
        {
            showing = false;
            return;
        }
        shown_at = now;
        sfx_play(SFX_SELECT);
    }
    float t = (float)(now - shown_at) / 1e6f, total = length / 1e6f;
    float in = t < 0.25f ? t / 0.25f : 1.0f, out = total - t < 0.4f ? (total - t) / 0.4f : 1.0f;
    float a = in < out ? in : out, slide = (1.0f - in) * (1.0f - in) * 80.0f;
    if (app.global.ra_popup_style == 1)
    {
        /* compact: one line at the top */
        char line[300];
        snprintf(line, sizeof(line), "%s  \xc2\xb7  %s", title, detail);
        float lw = text_width(22, FONT_REGULAR, line) + 90;
        if (lw > plat_width() - 200)
            lw = plat_width() - 200;
        float lx = (plat_width() - lw) * 0.5f, ly = 24 - slide * 0.5f;
        draw_rrect(lx, ly, lw, 52, 26, argb_alpha(TH_CARD_A(0xf0), a));
        icon_draw(ICON_TROPHY, lx + 18, ly + 11, 30, argb_alpha(TH_GOLD, a));
        text_draw_fit(lx + 60, ly + 13, 22, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, lw - 80, line);
        return;
    }
    if (app.global.ra_popup_style == 2)
    {
        /* big trophy, in the middle */
        float bw = 760, bh = 300, bx = (plat_width() - bw) * 0.5f, by = (plat_height() - bh) * 0.5f - 40 + slide * 0.4f;
        draw_rrect(bx, by, bw, bh, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
        draw_circle(bx + bw * 0.5f, by + 92, 62, argb_alpha(0xff2a2410u, a));
        icon_draw(ICON_TROPHY, bx + bw * 0.5f - 44, by + 48, 88, argb_alpha(TH_GOLD, a));
        text_draw_fit(bx + bw * 0.5f, by + 176, 34, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, bw - 60, title);
        text_draw_fit(bx + bw * 0.5f, by + 230, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, bw - 60,
                      detail);
        return;
    }
    const float w = 660, h = 116, x = plat_width() - w - 48 + slide, y = 48;
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
    draw_rrect(x + 18, y + 18, 80, 80, TH_RADIUS_SMALL, argb_alpha(0xff2a2410u, a));
    icon_draw(ICON_TROPHY, x + 30, y + 30, 56, argb_alpha(TH_GOLD, a));
    text_draw_fit(x + 120, y + 20, 30, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, w - 140, title);
    text_draw_fit(x + 120, y + 66, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_LEFT, w - 140,
                  detail);
}

/* ---------------------------------------------------------------- game start/stop */

static void draw_timer(void);

static bool speed_changed; /* fast forward or rewind: not the game's normal speed */

void app_draw_game(uint8_t dim)
{
    int w, h, fmt;
    size_t pitch;
    bool fresh;
    const void *pixels = host_frame(&w, &h, &pitch, &fmt, &fresh);
    if (pixels && fresh)
        plat_upload_game(pixels, w, h, pitch, fmt, app.settings.upscale, app.settings.upscale_filter);
    else if (!pixels && fresh && w > 0)
        plat_upload_game_gpu(w, h); /* the core rendered through Vulkan */
    Settings view = app.settings;
    if (play_widescreen_active())
        view.aspect = ASPECT_16_9;
    if (bezel_shown(&view))
        view.border = 0; /* the bezel's window is the full-height 4:3 picture */

    /* around the picture: black, the current background, or a TV */
    int gx, gy, gw, gh;
    plat_game_rect(&gx, &gy, &gw, &gh);
    /* Stretch fills the screen: no border or TV around it */
    if (view.border && !bezel_shown(&view) && view.aspect != ASPECT_STRETCH && gw > 0 && gw < plat_width() - 8)
    {
        float k = dim / 255.0f;
        if (view.border == 1)
        {
            shelf_backdrop();
            draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xff000000u, 0.45f + 0.55f * (1 - k)));
        }
        else
        {
            /* a 90s TV: dark room, plastic body, recessed screen, a power light */
            draw_rect(0, 0, plat_width(), plat_height(), argb_lerp(0xff000000u, 0xff0c0d14u, k));
            draw_rrect(gx - 74, gy - 58, gw + 148, gh + 130, 56, argb_lerp(0xff000000u, 0xff26262du, k));
            draw_rrect(gx - 34, gy - 58, gw + 68, 8, 4, argb_lerp(0xff000000u, 0xff36363fu, k));
            draw_rrect(gx - 26, gy - 24, gw + 52, gh + 48, 34, argb_lerp(0xff000000u, 0xff0e0e11u, k));
            draw_circle(gx + gw + 30, gy + gh + 44, 7, argb_lerp(0xff000000u, 0xff3cd070u, k));
            text_draw(gx + gw * 0.5f, gy + gh + 32, 22, FONT_BOLD, argb_lerp(0xff000000u, 0xff5a5a66u, k),
                      ALIGN_CENTER, SwanStationPS5_TITLE);
        }
    }
    plat_set_colour(view.brightness, view.colour, view.sharpen);
    plat_set_fsr(app.settings.fsr);
    {
        double core_hz = host_fps();
        double speed = app.fps > 1.0f && core_hz > 1.0 ? app.fps * 100.0 / core_hz : 100.0;
        plat_set_framegen(app.global.framegen, core_hz, speed, !speed_changed);
    }
    plat_draw_game(&view, host_aspect(), dim);
    bezel_draw(&view, dim);
    if (app.screen == SCREEN_GAME)
        draw_timer();
}

/* Is there a BIOS file (any .bin) in dir? When the folder can't be listed (sandboxed mode) we can't tell: yes. */
static bool bios_dir_has_bin(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return true;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)))
        found = str_icmp(path_ext(e->d_name), "bin") == 0;
    closedir(d);
    return found;
}

/* Is there a BIOS file (any .bin) in bios/? */
static bool bios_file_present(void)
{
    return bios_dir_has_bin(app.paths.bios);
}

/* Also support the BIOS folder of PSXS5: used when ours has no BIOS file and that one has. */
static void bios_use_legacy_dir(void)
{
    static const char legacy[] = "/data/PSXS5/bios";
    DIR *d = opendir(legacy);
    if (!d) /* missing or can't be listed: nothing proves it holds a BIOS */
        return;
    closedir(d);
    if (bios_dir_has_bin(app.paths.bios) || !bios_dir_has_bin(legacy))
        return;
    str_copy(app.paths.bios, sizeof(app.paths.bios), legacy);
    SwanStationPS5_log("bios: none in our bios folder, using %s", legacy);
}

/* A libcrypt game (most PAL discs) needs its .sbi file beside the disc image, with the image's name. The package holds
 * them (assets/sbi_files/<serial>.sbi: patches, from psxdatacenter): when the disc has none, its copy is put there. */

/* Spyro: Year of the Dragon and CTR have two pressings under one serial, each with its own .sbi: they are told apart by
 * the CRC-32 of the first .bin of the disc (Redump's). */
static const struct
{
    const char *serial;
    uint32_t crc;
    const char *file;
} SBI_BY_CRC[] = {
    {"SCES-02105", 0xf21229a3u, "SCES-02105_NoEDC.sbi"}, /* CTR, original */
    {"SCES-02105", 0x2a98762fu, "SCES-02105_EDC.sbi"},   /* CTR, Platinum */
    {"SCES-02835", 0x5d3785eeu, "SCES-02835_v1.0.sbi"},  /* Spyro, original */
    {"SCES-02835", 0x9bcfefe1u, "SCES-02835_v1.1.sbi"},  /* Spyro, Platinum */
};

/* CRC-32 of a file; `permille` (0..1000) says how far it is, for the bar */
static bool crc32_of_file(const char *path, uint32_t *out, SDL_atomic_t *permille)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    static uint8_t buf[256 * 1024];
    uint32_t crc = 0xffffffffu;
    size_t n;
    long done = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
        done += (long)n;
        SDL_AtomicSet(permille, total > 0 ? (int)(done * 1000 / total) : 0);
    }
    fclose(f);
    *out = ~crc;
    return true;
}

/* The disc's first .bin: the image itself, or the first FILE of its .cue (beside it) */
static bool first_bin_of(const char *image, char *bin, size_t size)
{
    const char *ext = path_ext(image);
    if (!strcasecmp(ext, "bin"))
    {
        str_copy(bin, size, image);
        return true;
    }
    if (strcasecmp(ext, "cue"))
        return false;
    FILE *f = fopen(image, "rb");
    if (!f)
        return false;
    char line[512], name[SwanStationPS5_PATH_MAX] = "";
    while (fgets(line, sizeof(line), f))
    {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (strncasecmp(p, "FILE", 4))
            continue;
        const char *q = strchr(p, '"'), *e = q ? strchr(q + 1, '"') : NULL;
        if (!e)
            break;
        size_t len = (size_t)(e - q - 1);
        if (len < sizeof(name))
        {
            memcpy(name, q + 1, len);
            name[len] = '\0';
        }
        break;
    }
    fclose(f);
    if (!name[0])
        return false;
    str_copy(bin, size, image);
    char *slash = strrchr(bin, '/');
    size_t dir = slash ? (size_t)(slash - bin) + 1 : 0;
    if (dir + strlen(name) >= size)
        return false;
    strcpy(bin + dir, name);
    return true;
}

/* The CRC-32 runs in a thread, with a bar on screen meanwhile; the game starts when it is done */
static struct
{
    bool active;
    SDL_atomic_t done, permille;
    bool ok;
    uint32_t crc;
    char bin[SwanStationPS5_PATH_MAX], serial[16], target[SwanStationPS5_PATH_MAX];
    int game;
    bool resume;
} SC;
static int sbi_checked_game = -1; /* its CRC was read: start it, whatever the result was */

static int sbi_crc_thread(void *unused)
{
    (void)unused;
    SC.ok = crc32_of_file(SC.bin, &SC.crc, &SC.permille);
    SDL_AtomicSet(&SC.done, 1);
    return 0;
}

/* The copy that matches the CRC-32, once it is known */
static void sbi_finish(void)
{
    char name[64], source[SwanStationPS5_PATH_MAX];
    if (!SC.ok)
        return;
    for (size_t i = 0; i < sizeof(SBI_BY_CRC) / sizeof(SBI_BY_CRC[0]); ++i)
    {
        if (strcmp(SBI_BY_CRC[i].serial, SC.serial) || SBI_BY_CRC[i].crc != SC.crc)
            continue;
        snprintf(name, sizeof(name), "sbi_files/%s", SBI_BY_CRC[i].file);
        plat_asset_path(source, sizeof(source), name);
        file_copy(source, SC.target);
        return;
    }
}

/* false when the CRC-32 is being read: the game starts when it is done */
static bool sbi_prepare(int index, bool resume)
{
    const Game *g = &app.library.games[index];
    if (index == sbi_checked_game)
    {
        sbi_checked_game = -1;
        return true;
    }
    if (SC.active || !g->serial[0] || !strcasecmp(path_ext(g->path), "m3u"))
        return !SC.active;
    char target[SwanStationPS5_PATH_MAX], name[64], source[SwanStationPS5_PATH_MAX];
    str_copy(target, sizeof(target), g->path);
    char *slash = strrchr(target, '/'), *dot = strrchr(target, '.');
    if (!dot || (slash && dot < slash) || (size_t)(dot - target) + 5 > sizeof(target))
        return true;
    strcpy(dot, ".sbi");
    FILE *have = fopen(target, "rb");
    if (have)
    {
        fclose(have);
        return true;
    }
    snprintf(name, sizeof(name), "sbi_files/%s.sbi", g->serial);
    plat_asset_path(source, sizeof(source), name);
    if (file_copy(source, target))
        return true;
    /* a serial with two pressings: the disc's CRC-32 says which */
    bool listed = false;
    for (size_t i = 0; i < sizeof(SBI_BY_CRC) / sizeof(SBI_BY_CRC[0]); ++i)
        listed |= !strcmp(SBI_BY_CRC[i].serial, g->serial);
    if (!listed || !first_bin_of(g->path, SC.bin, sizeof(SC.bin)))
        return true;
    str_copy(SC.serial, sizeof(SC.serial), g->serial);
    str_copy(SC.target, sizeof(SC.target), target);
    SC.game = index;
    SC.resume = resume;
    SC.ok = false;
    SDL_AtomicSet(&SC.done, 0);
    SDL_AtomicSet(&SC.permille, 0);
    SDL_Thread *t = SDL_CreateThread(sbi_crc_thread, "sbi-crc", NULL);
    if (!t)
        return true; /* no thread: no patch, the box says why the game does not start */
    SDL_DetachThread(t);
    SC.active = true;
    return false;
}

/* Every frame while it runs: the buttons stay out of the screens below; when it is done, the game starts */
static bool sbi_check_input(uint32_t *pressed)
{
    if (!SC.active)
        return false;
    *pressed = 0;
    if (SDL_AtomicGet(&SC.done))
    {
        SC.active = false;
        sbi_finish();
        sbi_checked_game = SC.game;
        app_start_game(SC.game, SC.resume);
    }
    return true;
}

/* The bar, as the one that comes up when O is held to close the app: a panel with the message and the progress */
static void sbi_check_draw(void)
{
    if (!SC.active)
        return;
    const char *msg = tr("Extracting the CRC-32 to try to apply the LibCrypt patch...");
    float w = text_width(26, FONT_REGULAR, msg) + 96, h = 84;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - 300;
    float p = SDL_AtomicGet(&SC.permille) / 1000.0f;
    draw_rrect(x, y, w, h, TH_RADIUS_SMALL, 0xf0000000u | (TH_PILL & 0xffffffu));
    text_draw(plat_width() * 0.5f, y + 12, 26, FONT_REGULAR, TH_TEXT, ALIGN_CENTER, msg);
    draw_rrect(x + 32, y + 56, w - 64, 12, 6, TH_DIVIDER);
    draw_rrect(x + 32, y + 56, fmaxf(12.0f, (w - 64) * fminf(p, 1.0f)), 12, 6, TH_FOCUS);
}

void app_start_game(int index, bool resume)
{
    if (index < 0 || index >= app.library.count)
        return;
    if (!sbi_prepare(index, resume))
        return;
    const Game *g = &app.library.games[index];
    char error[480], own[SwanStationPS5_PATH_MAX];
    app_game_config_path(own, sizeof(own), g);
    app.game_has_own = config_load_game(&app.settings, &app.global, own);
    bool translated = play_prepare_patch(g);
    play_rewind_reset();
    controls_start(g, &app.settings);
    app.play_seconds = 0;
    host_set_gun(controls_gun_for(g, &app.settings));
    host_set_special(controls_special_for(g, &app.settings));
    {
        const GameInfo *info = gamedb_get(g->serial);
        unsigned fixes = info && app.settings.game_fixes ? info->flags & GDB_FIXES : 0;
        /* a DualShock starts in digital mode until its ANALOG button is pressed;
         * games that use the sticks get it in analog mode (Gran Turismo...) */
        if (info && (info->flags & GDB_ANALOG) && !(info->flags & GDB_NO_AUTO_ANALOG))
            fixes |= GDB_ANALOG;
        host_set_fixes(fixes);
        if (fixes)
            SwanStationPS5_log("start: known fixes %x (DuckStation's database)", fixes);
    }
    SwanStationPS5_log("start: %s (%s) from %s%s", g->title, g->serial, g->path,
              app.game_has_own ? " with its own settings" : "");
    if (!host_load(g->path, g->serial, &app.paths, &app.settings, error, sizeof(error)))
    {
        controls_stop();
        app.settings = app.global;
        if (strstr(error, "libcrypt protection"))
            app_message_box(error); /* it stays until X: it says what to do */
        else
            app_toast(error);
        return;
    }
    app.game = g;
    app.game_index = index;
    app.global.last_game = index;
    config_save(&app.global, app.paths.config);
    GameStats *st = stats_get(g->id);
    if (st)
    {
        st->last_played = (int64_t)time(NULL);
        stats_save();
    }
    if (!bios_file_present()) /* after the load: it blocks, and the notice would run out before it was drawn */
        app_toast_for("BIOS not found, SCPH1001.BIN is recommended due to the low compatibility of OpenBios.", 10.0f);
    if (app.settings.renderer == 1)
        app_toast_for("Rendering on the CPU, using the GPU is recommended.", 5.0f);
    app_toast_for("Press [L3] + [R3] to open the game menu...", 5.0f);
    plat_audio_open(host_sample_rate());
    plat_audio_clear();
    ra_game_loaded(g->path);
    if (ra_hardcore())
        cheats_clear(&app.cheats); /* hardcore: no cheats */
    else if (cheats_load(&app.cheats, g, app.paths.cheats))
    {
        cheats_apply(&app.cheats);
        int on = 0;
        for (int i = 0; i < app.cheats.count; ++i)
            on += app.cheats.items[i].enabled;
        if (on)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), tr(on == 1 ? "%d cheat active" : "%d cheats active"), on);
            app_toast(msg);
        }
    }
    if (play_widescreen())
        app_toast("Widescreen on");
    else if (app.settings.widescreen && !ra_hardcore())
        app_toast("This game has no widescreen code: playing in 4:3");
    if (translated)
        app_toast("Translation patch applied");
    if (resume && play_load_resume())
        app_toast("Continuing where you left off...");
    if (controls_gun_active())
        app_toast("Light gun: point the controller at the screen, R2 fires, R3 re-centres");
    app.screen = SCREEN_GAME;
}

void app_stop_game(void)
{
    play_save_resume(false);
    play_rewind_reset();
    int unlocked, total;
    GameStats *st = app.game ? stats_get(app.game->id) : NULL;
    if (st && ra_game_progress(&unlocked, &total))
    {
        st->ach_unlocked = unlocked;
        st->ach_total = total;
    }
    stats_save();
    ra_game_unloaded();
    controls_stop();
    host_unload();
    plat_audio_open(UI_RATE);
    plat_audio_clear();
    cheats_clear(&app.cheats);
    app.game = NULL;
    app.game_has_own = false;
    app.settings = app.global;
    app.screen = SCREEN_LIBRARY;
}

void app_open_settings(enum Screen back_to)
{
    app.settings_return = back_to;
    app.screen = SCREEN_SETTINGS;
    settings_opened();
}

/* ---------------------------------------------------------------- emulation */

/* The player's button mapping: each controller button presses its mapped PS1
 * button (Settings > Controls > Button mapping). */
static uint32_t map_buttons(uint32_t buttons)
{
    uint32_t out = buttons & ~0xffffu;
    for (int b = 0; b < 16; ++b)
        if (buttons & BIT(b))
        {
            int to = app.settings.button_map[b];
            if (to >= 0 && to < 16)
                out |= BIT(to);
        }
    return out;
}

/* Speedrun timer: touchpad + Triangle starts/pauses, touchpad + Circle resets.
 * It counts only while the game runs (not in the SwanStationPS5 menu). */
static struct
{
    bool shown, running;
    double seconds;
} timer;

static void draw_timer(void)
{
    if (!timer.shown)
        return;
    int cs = (int)(timer.seconds * 100.0);
    char text[32];
    if (cs >= 360000)
        snprintf(text, sizeof(text), "%d:%02d:%02d.%02d", cs / 360000, cs / 6000 % 60, cs / 100 % 60, cs % 100);
    else
        snprintf(text, sizeof(text), "%02d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
    float w = text_width(34, FONT_BOLD, "00:00:00.00") + 48;
    draw_rrect(40, 40, w, 64, TH_RADIUS_SMALL, TH_CARD_A(0xd0));
    text_draw(40 + w * 0.5f, 50, 34, FONT_BOLD, timer.running ? TH_TEXT : TH_GOLD, ALIGN_CENTER, text);
}

/* The DualSense light bars: per player, or the cover's colour. */
static void update_lightbars(void)
{
    static const uint32_t players[SwanStationPS5_MAX_PADS] = {0x2050ff, 0xff2030, 0x20d040, 0xff40c0};
    if (app.settings.lightbar == 0)
        return;
    uint32_t cover = app.game ? covers_color(app.game_index) & 0xffffff : 0;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        plat_set_lightbar(i, app.settings.lightbar == 2 && cover ? cover : players[i]);
}

static float auto_since, auto_shown; /* auto-save: time since the last, and its icon */

/* Buttons still held from the menu when the game comes back (the Cross that
 * chose Resume): the game doesn't see them until they're let go. */
static uint32_t held_from_menu;

static void game_screen(PadState *pads)
{
    uint32_t held_now = 0;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        held_now |= pads[i].buttons;
    held_from_menu &= held_now;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        pads[i].buttons &= ~held_from_menu;
    static uint32_t combo_prev;
    uint32_t combo = pads[0].buttons & (BIT(BTN_L3) | BIT(BTN_R3));
    bool combo_hit = combo == (BIT(BTN_L3) | BIT(BTN_R3)) && combo_prev != combo;
    combo_prev = combo;

    /* Touchpad: a tap is the PS1's Select (the PS5 SDL driver has no Create
     * button); the menu is L3 + R3. */
    static uint64_t touch_since;
    static bool touch_used;
    static int select_frames;
    bool touch = (pads[0].buttons | pads[1].buttons) & BIT(BTN_MENU);
    uint64_t t_now = plat_ticks_us();
    bool open_menu = combo_hit;
    /* touchpad held + R2: fast forward; + L2: rewind */
    uint32_t all = pads[0].buttons | pads[1].buttons;
    bool fast = touch && (all & BIT(BTN_R2)), back = touch && (all & BIT(BTN_L2));
    speed_changed = fast || back;
    if (fast || back)
        touch_used = true; /* a combo, not Select or the menu */
    /* touchpad + Square: screenshot; + Triangle / Circle: timer; + R1: next disc */
    static uint32_t combo_all_prev;
    uint32_t newly = touch ? all & ~combo_all_prev : 0;
    combo_all_prev = all;
    const uint32_t combo_keys = BIT(BTN_SQUARE) | BIT(BTN_TRIANGLE) | BIT(BTN_CIRCLE) | BIT(BTN_R1);
    if (touch && (all & combo_keys))
        touch_used = true;
    if (newly & BIT(BTN_SQUARE))
    {
        bool ok = play_screenshot();
        app_toast(ok ? "Screenshot saved in /data/SwanStationPS5/screenshots" : "Could not save the screenshot");
        sfx_play(ok ? SFX_SELECT : SFX_BACK);
    }
    if (newly & BIT(BTN_TRIANGLE))
    {
        timer.shown = true;
        timer.running = !timer.running;
        sfx_play(SFX_CLICK);
    }
    if (newly & BIT(BTN_CIRCLE))
    {
        timer.running = false;
        timer.seconds = 0;
        timer.shown = false;
        app_toast("Timer reset");
    }
    if ((newly & BIT(BTN_R1)) && host_disc_count() > 1)
    {
        int next = (host_disc_index() + 1) % host_disc_count();
        if (host_disc_select(next))
        {
            char msg[64];
            snprintf(msg, sizeof(msg), tr("Disc %d of %d inserted"), next + 1, host_disc_count());
            app_toast(msg);
            sfx_play(SFX_SELECT);
        }
    }
    if (touch)
    {
        if (!touch_since)
            touch_since = t_now;
        else if (!touch_used && t_now - touch_since > 500000)
        {
            touch_used = true; /* held for a while: not a tap, not Select */
        }
    }
    else
    {
        if (touch_since && !touch_used)
            select_frames = 6; /* ~100 ms: long enough for every game to see it */
        touch_since = 0;
        touch_used = false;
    }
    if (open_menu)
    {
        menu_open();
        app_draw_game(255);
        return;
    }
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        if (fast || back)
            pads[i].buttons &= ~(BIT(BTN_L2) | BIT(BTN_R2));
        if (touch) /* the combo buttons go to SwanStationPS5, not the game */
            pads[i].buttons &= ~combo_keys;
        pads[i].buttons = map_buttons(pads[i].buttons & ~BIT(BTN_MENU));
        if (i == 0 && select_frames > 0)
        {
            pads[0].buttons |= BIT(BTN_SELECT);
            --select_frames;
        }
        /* A digital pad has no sticks: let the left stick drive the D-pad. With a
         * DualShock, Auto leaves that to the core (only while the game keeps it in
         * digital mode: AnalogDPadInDigitalMode in host.c), Always does it here. */
        bool map = app.settings.stick_dpad == STICK_DPAD_ALWAYS ||
                   (app.settings.stick_dpad == STICK_DPAD_AUTO && !app.settings.analog);
        if (map)
        {
            const int16_t dead = 16000;
            if (pads[i].lx < -dead) pads[i].buttons |= BIT(BTN_LEFT);
            if (pads[i].lx > dead) pads[i].buttons |= BIT(BTN_RIGHT);
            if (pads[i].ly < -dead) pads[i].buttons |= BIT(BTN_UP);
            if (pads[i].ly > dead) pads[i].buttons |= BIT(BTN_DOWN);
        }
    }
    controls_apply(pads, &app.settings, app.dt);
    host_set_pads(pads);

    /* Pace by the audio queue: the core's 59.94/50 Hz never matches the TV
     * exactly, so run 0-2 frames per refresh to keep ~3 frames of sound queued. */
    static uint64_t last_us, fps_window_start;
    static int fps_frames;
    static uint32_t fps_news_at, fps_repeats_at;
    uint64_t now = plat_ticks_us();
    size_t per_frame = (size_t)(host_sample_rate() / host_fps());
    size_t queued = plat_audio_queued_frames();
    int runs = 1;
    if (queued < per_frame * 2)
        runs = 2;
    else if (queued > per_frame * 5)
        runs = 0;
    static bool rewind_hint;
    if (back)
    {
        /* step back two recorded moments a second... per refresh: smooth enough */
        static int pace;
        if (++pace >= 3)
        {
            pace = 0;
            if (play_rewind_step())
                host_run_frame(); /* draws the restored moment */
            else if (!app.settings.rewind && !rewind_hint)
            {
                app_toast("Turn on Rewind in Settings > System");
                rewind_hint = true;
            }
        }
        plat_audio_clear();
        runs = 0;
    }
    else if (fast && !ra_hardcore())
    {
        runs = 4;
        if (queued > per_frame * 3)
            plat_audio_clear(); /* keep up instead of queueing sound */
    }
    if (timer.running)
        timer.seconds += app.dt;
    update_lightbars();
    static uint64_t emu_us;
    static int emu_frames;
    uint64_t emu_start = plat_ticks_us();
    /* Run-ahead: run the real frame, keep it, run 1 or 2 more to show where the
     * game is about to be (silently), then go back to the real one. What's on
     * screen answers the buttons that many frames sooner. */
    static void *ahead_state;
    static size_t ahead_size;
    /* run-ahead shows frames of a look-ahead that are not the game's own: Double frames needs the real ones */
    int ahead = !fast && !back && !app.global.framegen ? app.settings.run_ahead : 0;
    if (ahead > 0 && runs > 0 && (!ahead_state || host_state_size() != ahead_size))
    {
        free(ahead_state); /* another game's states are another size */
        ahead_size = host_state_size();
        ahead_state = ahead_size ? malloc(ahead_size) : NULL;
    }
    for (int i = 0; i < runs; ++i)
    {
        host_run_frame();
        ra_frame();
        play_rewind_record();
        if (ahead > 0 && i == runs - 1 && ahead_state && host_state_size() == ahead_size &&
            host_serialize(ahead_state, ahead_size))
        {
            host_set_speculative(true);
            for (int k = 0; k < ahead; ++k)
                host_run_frame();
            host_unserialize(ahead_state, ahead_size);
            host_set_speculative(false);
        }
    }
    /* quick resume: a background save every three minutes */
    static float since_resume;
    since_resume += app.dt;
    if (since_resume > 180.0f)
    {
        since_resume = 0;
        play_save_resume(true);
    }
    /* auto-save: every 5, 10 or 15 minutes of play, the oldest of three slots */
    if (app.play_seconds < app.dt * 2)
        auto_since = 0; /* a game just started */
    app.play_seconds += app.dt;
    if (app.settings.autosave > 0 && !ra_hardcore())
    {
        auto_since += app.dt;
        if (auto_since > app.settings.autosave * 300.0f)
        {
            auto_since = 0;
            if (play_save_auto())
                auto_shown = 2.0f;
        }
    }
    emu_us += plat_ticks_us() - emu_start;
    emu_frames += runs;
    if (emu_frames >= 240)
    {
        SwanStationPS5_log("emu: %.1f ms per emulated frame, %.1f fps measured, the game's own frames %u new, %u repeated",
                  emu_us / 1000.0 / emu_frames, app.fps, host_frames_new(), host_frames_repeated());
        emu_us = 0;
        emu_frames = 0;
    }
    fps_frames += runs;
    if (now - fps_window_start >= 1000000)
    {
        app.fps = fps_frames * 1000000.0f / (float)(now - fps_window_start);
        /* the game's own frames among them: a 30 fps game on a 60 fps core repeats every picture */
        uint32_t news = host_frames_new(), repeats = host_frames_repeated();
        uint32_t all_frames = news - fps_news_at + repeats - fps_repeats_at;
        app.real_fps = all_frames ? app.fps * (float)(news - fps_news_at) / (float)all_frames : app.fps;
        fps_news_at = news;
        fps_repeats_at = repeats;
        fps_frames = 0;
        fps_window_start = now;
    }
    if (runs == 0 && now - last_us < 4000)
        plat_sleep_us(1000);
    last_us = now;

    app_draw_game(255);
    controls_draw();
    if (auto_shown > 0)
    {
        /* a quiet sign that an auto-save was written */
        auto_shown -= app.dt;
        float a = auto_shown > 1.6f ? (2.0f - auto_shown) / 0.4f : auto_shown < 0.5f ? auto_shown / 0.5f : 1.0f;
        draw_rrect(plat_width() - 96, plat_height() - 96, 64, 64, 32, argb_alpha(0xb0000000u, a));
        icon_draw(ICON_DEVICE_FLOPPY, plat_width() - 82, plat_height() - 82, 36, argb_alpha(TH_TEXT, a));
    }
    if (app.settings.show_fps)
    {
        char f[64];
        int at;
        if (app.real_fps > 0.0f && app.fps - app.real_fps >= 1.0f)
            at = snprintf(f, sizeof(f), "%.1f FPS (%s %.1f)", app.fps, tr("game"), app.real_fps);
        else
            at = snprintf(f, sizeof(f), "%.1f FPS", app.fps);
        /* frame interpolation at work: the rate of the screen */
        double fg_hz = plat_framegen_hz();
        if (fg_hz > 1.0 && at > 0 && at < (int)sizeof(f) - 12)
        {
            /* the frames the eye gets: each of the game's own and one generated, up to the screen's rate (25 -> 50) */
            double own = app.real_fps > 1.0f ? app.real_fps : app.fps;
            double shown = own * 2.0 < fg_hz ? own * 2.0 : fg_hz;
            snprintf(f + at, sizeof(f) - (size_t)at, " | FG %.0f", shown);
        }
        float w = text_width(26, FONT_BOLD, f) + 40;
        draw_rrect(24, 24, w, 52, 26, 0xb0000000u);
        text_draw(44, 36, 26, FONT_BOLD, TH_GOOD, ALIGN_LEFT, f);
    }
    if (fast || back)
    {
        int icon = back ? ICON_HISTORY : ICON_PLAYER_TRACK_NEXT;
        draw_rrect(plat_width() - 110, 30, 80, 60, 30, 0xb0000000u);
        icon_draw(icon, plat_width() - 92, 38, 44, TH_TEXT);
    }
    app_draw_toast();
}

/* Play time: counted while a game runs (not while its menu is open). */
static void count_play_time(void)
{
    static float seconds;
    static uint64_t saved_at;
    if (!app.game || app.screen != SCREEN_GAME)
        return;
    seconds += app.dt;
    if (seconds < 1.0f)
        return;
    GameStats *st = stats_get(app.game->id);
    if (st)
        st->seconds += (uint32_t)seconds;
    seconds -= (float)(uint32_t)seconds;
    uint64_t now = plat_ticks_us();
    if (now - saved_at > 60000000) /* write it down once a minute */
    {
        int unlocked, total;
        if (st && ra_game_progress(&unlocked, &total))
        {
            st->ach_unlocked = unlocked;
            st->ach_total = total;
        }
        stats_save();
        saved_at = now;
    }
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    /* First thing: proves the loader accepted the title and main() runs. */
    plat_notify("SwanStationPS5 " SwanStationPS5_VERSION " starting...");
    char root[SwanStationPS5_PATH_MAX];
    plat_default_root(root, sizeof(root));
    config_paths(&app.paths, root);

    /* Unlock before SDL starts any thread: the HEN changes this process's
     * credentials, which Porpoise did not survive with threads running. */
    app.sandboxed = !plat_prepare_storage(app.sandbox_reason, sizeof(app.sandbox_reason));
    /* An empty file called reset-settings, beside the settings file, asks for clean settings by hand: removed before
     * anything reads it (and the signed-in profile's once the profiles are known). */
    char reset_marker[SwanStationPS5_PATH_MAX];
    path_join(reset_marker, sizeof(reset_marker), app.paths.root, "reset-settings");
    const bool settings_reset = path_exists(reset_marker);
    if (settings_reset)
        remove(app.paths.config);
    /* A new version after settings written by another: the first screen asks whether to start from its factory
     * settings (the files of versions before 1.1.4 carry no version). */
    char saved_version[32] = "";
    const bool version_changed =
        !settings_reset && path_exists(app.paths.config) &&
        (!config_peek_string(app.paths.config, "version", saved_version, sizeof(saved_version)) ||
         strcmp(saved_version, SwanStationPS5_VERSION) != 0);
    /* frame interpolation wants the 120 Hz mode, chosen when the screen opens */
    plat_want_high_refresh(config_peek_bool(app.paths.config, "framegen"));
    /* the shelf and menus on the GPU, unless Settings > System > SwanStationPS5 turned that off */
    plat_want_gpu_ui(config_peek_bool_or(app.paths.config, "gpu_ui", true));
    plat_want_output(config_peek_int(app.paths.config, "output_res", 2));

    if (!plat_init())
    {
        char msg[300];
        snprintf(msg, sizeof(msg), "SwanStationPS5: %s", plat_init_error());
        plat_notify(msg);
        for (;;)
            plat_sleep_us(1000000);
    }
    if (!text_init())
        plat_notify("SwanStationPS5: interface font missing from assets/fonts");

    /* Sandboxed or not, files in /data open and save; only listing differs. */
    const char *dirs[] = {app.paths.root,   app.paths.games,  app.paths.bios,
                          app.paths.saves,  app.paths.states, app.paths.cheats,
                          app.paths.covers, app.paths.logs,   app.paths.cache};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        make_dirs(dirs[i]);
    bios_use_legacy_dir();
    char probe[SwanStationPS5_PATH_MAX];
    path_join(probe, sizeof(probe), app.paths.root, ".write-test");
    FILE *pf = fopen(probe, "w");
    if (pf)
    {
        fclose(pf);
        remove(probe);
    }
    else
        snprintf(app.storage_error, sizeof(app.storage_error),
                 "%s isn't writable. Is the HEN running? (%s)", app.paths.root, app.sandbox_reason);
    char log_path[SwanStationPS5_PATH_MAX];
    path_join(log_path, sizeof(log_path), app.paths.logs, "SwanStationPS5.log");
    SwanStationPS5_log_open(log_path);
    ps5_crash_install(log_path);
#if defined(__PROSPERO__)
    /* the log's stderr lines go to klog: nc <PS5 IP> 3232 shows them live */
    extern int ps5_klog_capture_stderr(const char *prefix);
    ps5_klog_capture_stderr("");
#endif
    /* a second games folder on the extended storage, only when it is mounted */
    struct stat ext;
    if (stat("/mnt/ext1", &ext) != 0 || !S_ISDIR(ext.st_mode))
        SwanStationPS5_log("games folder /mnt/ext1/SwanStationPS5/games: /mnt/ext1 is not available");
    else if (stat("/mnt/ext1/SwanStationPS5/games", &ext) == 0)
        SwanStationPS5_log("games folder /mnt/ext1/SwanStationPS5/games: already exists");
    else if (make_dirs("/mnt/ext1/SwanStationPS5/games"))
        SwanStationPS5_log("games folder /mnt/ext1/SwanStationPS5/games: created successfully");
    else
        SwanStationPS5_log("games folder /mnt/ext1/SwanStationPS5/games: could not be created (%s)", strerror(errno));
    SwanStationPS5_log("SwanStationPS5 %s starting, data root %s", SwanStationPS5_VERSION, app.paths.root);
    SwanStationPS5_log("screen: %s", plat_screen_info());
#if defined(__PROSPERO__)
    extern size_t ps5_heap_size_mb(void);
    SwanStationPS5_log("heap: %zu MB of direct memory%s", ps5_heap_size_mb(),
              ps5_heap_size_mb() ? "" : " (unavailable: using the system heap)");
#endif
    SwanStationPS5_log(app.sandboxed ? "storage: sandboxed (%s)" : "storage: unlocked%s",
              app.sandboxed ? app.sandbox_reason : "");
    SwanStationPS5_log("storage probe before unlock: %s", plat_sandbox_probe());
    vk_probe(app.paths.root); /* v2: proves the Vulkan driver runs; logs only */
    app.unlock_setting = !plat_unlock_disabled();

    if (!app.storage_error[0])
        profiles_startup(); /* whose saves, settings and stats */
    if (settings_reset)
    {
        remove(app.paths.config);
        remove(reset_marker);
        SwanStationPS5_log("settings: clean start, asked for by hand");
    }
    if (version_changed)
    {
        SwanStationPS5_log("settings: saved by %s, this is %s: asking", saved_version[0] ? saved_version : "an older version",
                           SwanStationPS5_VERSION);
        reset_question_open();
    }
    config_load(&app.global, app.paths.config);
    app.settings = app.global;
    i18n_set(app.global.language);
    theme_apply(app.global.theme);
    stats_load(app.paths.user);
    plat_audio_open(UI_RATE);
    sfx_init(UI_RATE);
    sfx_configure(app.global.ui_sound, (app.global.ui_volume + 1) * 25);
    if (!app.storage_error[0])
    {
        ra_init(&app.paths);
        gamedb_start(); /* DuckStation's game database, downloaded once */
    }
    remote_update(app.global.remote && !app.storage_error[0]);
    if (app.global.update_check && !app.storage_error[0])
        update_check();
    shelf_init(app.global.last_game);
    if (!app.storage_error[0])
        app_rescan();
    shelf_select_game(app.global.last_game);
    plat_notify("SwanStationPS5 ready");

    bool quit = false;
    PadState pads[SwanStationPS5_MAX_PADS];
    uint64_t last = plat_ticks_us();
    const uint64_t opened = last;
    bool auto_update_tried = false;
    while (!quit)
    {
        uint64_t now = plat_ticks_us();
        app.dt = (float)(now - last) / 1e6f;
        if (app.dt > 0.1f)
            app.dt = 0.1f;
        last = now;

        /* An update found at start installs by itself, once, a second after the app opened (or when found, if later) */
        if (!auto_update_tried && update_state() == UPDATE_AVAILABLE && now - opened >= 1000000)
        {
            auto_update_tried = true;
            app_toast_for("An update is available and will be installed on your next restart.", 4.0f);
            update_install();
        }

        plat_poll(pads, &quit);
        if (app.quit_requested)
            quit = true;
        memcpy(app.pads, pads, sizeof(app.pads));
        uint32_t pressed = nav_pressed(pads);
        if (!question_input(&pressed) && !sbi_check_input(&pressed))
            message_box_input(&pressed);
        /* the shelf and settings paint a full-screen backdrop: no clear needed */
        bool backdrop = app.screen == SCREEN_LIBRARY || (app.screen == SCREEN_SETTINGS && !app.game);
        plat_begin_frame(backdrop ? 0 : 0xff000000u);
        static const char *const screen_names[] = {"library", "game",         "menu",
                                                   "settings", "cheats", "achievements",
                                                   "memory cards", "library stats", "manual",
                                                   "cheat search", "guide", "profile", "sign in"};
        ps5_crash_step(screen_names[app.screen]);
        static enum Screen last_screen = SCREEN_LIBRARY;
        enum Screen this_screen = app.screen;
        switch (app.screen)
        {
        case SCREEN_LIBRARY: shelf_screen(pressed); break;
        case SCREEN_GAME:
            if (last_screen != SCREEN_GAME)
                for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
                    held_from_menu |= pads[i].buttons;
            game_screen(pads);
            break;
        case SCREEN_MENU: menu_screen(pressed); break;
        case SCREEN_SETTINGS: settings_screen(pressed); break;
        case SCREEN_CHEATS: cheats_screen(pressed); break;
        case SCREEN_ACHIEVEMENTS: achievements_screen(pressed); break;
        case SCREEN_MEMCARDS: memcards_screen(pressed); break;
        case SCREEN_STATS: library_stats_screen(pressed); break;
        case SCREEN_MANUAL: manual_screen(pressed); break;
        case SCREEN_CHEAT_SEARCH: cheat_search_screen(pressed); break;
        case SCREEN_GUIDE: guide_screen(pressed); break;
        case SCREEN_PROFILE: profile_screen(pressed); break;
        case SCREEN_RA_LOGIN: login_screen(pressed); break;
        case SCREEN_TEXT_INPUT: textinput_screen(pressed); break;
        default: break;
        }
        last_screen = this_screen;
        count_play_time();
        remote_frame();
        /* a game's cheat file arrived from libretro-database */
        if (cheats_fetch_finished() && app.game && !ra_hardcore() &&
            cheats_load(&app.cheats, app.game, app.paths.cheats))
        {
            cheats_apply(&app.cheats);
            play_widescreen(); /* the file may hold the game's widescreen code */
        }
        if (app.screen != SCREEN_GAME)
            ra_idle();
        draw_achievement();
        sbi_check_draw();
        message_box_draw();
        question_draw();
        plat_end_frame();
    }

    /* Only the desktop build gets here; the PS5 shell closes the title. */
    ps5_crash_step("shutdown");
    SwanStationPS5_log("shutdown: leaving the main loop");
    if (app.game)
        app_stop_game();
    config_save(&app.global, app.paths.config);
    stats_save();
    SwanStationPS5_log("shutdown: settings and stats saved");
    remote_update(false);
    ra_shutdown();
    SwanStationPS5_log("shutdown: network and RetroAchievements stopped");
    covers_stop();
    plat_audio_close();
    icons_shutdown();
    text_shutdown();
    library_free(&app.library);
    SwanStationPS5_log("shutdown: interface freed");
    plat_shutdown();
    SwanStationPS5_log("shutdown: done");
#if defined(__PROSPERO__)
    /* The system closes the title: exit()/_exit() raise SIGSYS on the console. */
    extern int sceSystemServiceGetAppIdOfRunningBigApp(void);
    extern int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
    fflush(NULL);
    sceSystemServiceKillApp(sceSystemServiceGetAppIdOfRunningBigApp(), -1, 0, 0);
    for (;;)
        plat_sleep_us(100000); /* until the shell takes the title down */
#endif
    return 0;
}
