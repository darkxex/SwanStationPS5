/*
 * SwanStationPS5 - PlayStation X Super 5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Entry point, game start/stop and the emulation screen. The other screens
 * live in src/ui (shelf, settings, in-game menu and cheats).
 * Emulation is SwanStation, linked statically and driven through libretro.
 */
#include "ntfs/ntfsfs.h"
#include "drives.h"
#include "app.h"

#include "config.h"
#include "core/host.h"
#include "core/swanstation_options.h"
#include "bezels.h"
#include "controls.h"
#include "gamedb.h"
#include "profiles.h"
#include "covers.h"
#include "disceta.h"
#include "discimport.h"
#include "i18n.h"
#include "platform/platform.h"
#include "platform/ps5_crash.h"
#include "platform/vk/vk_probe.h"
#include "nfs/nfs.h"
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
    SwanStationPS5_log("%s", message);
    if (app.game && app.global.nostalgia && (app.screen == SCREEN_GAME || app.screen == SCREEN_LIBRARY))
        return;
    uint64_t length_us = (uint64_t)(seconds * 1e6f);
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

/* The screens call app_draw_toast() as they always did; it draws nothing there: the main loop draws the toast once,
 * after everything (the import card, the boxes), so it is never behind one. */
void app_draw_toast(void)
{
}

static void toast_render(void)
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
    /* a line break in the message makes a second line */
    char first[sizeof(toast)];
    const char *second = strchr(toast, '\n');
    size_t first_len = second ? (size_t)(second - toast) : strlen(toast);
    memcpy(first, toast, first_len);
    first[first_len] = '\0';
    if (second)
        ++second;
    const float line_gap = 36;
    float w = toast_runs(first, 0, 0, 0, false), h = second ? 64 + line_gap : 64;
    if (second)
    {
        float w2 = toast_runs(second, 0, 0, 0, false);
        w = w2 > w ? w2 : w;
    }
    w += 96;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - 190 - (second ? line_gap : 0) + rise;
    draw_rrect(x, y, w, h, h * 0.5f > 32 ? 32 : h * 0.5f, argb_alpha(TH_PILL_A(0xf0), a));
    icon_draw(ICON_INFO_CIRCLE, x + 22, y + h * 0.5f - 14, 28, argb_alpha(TH_FOCUS, a));
    draw_set_glyph_alpha(a); /* the pictures of the buttons fade with the text */
    toast_runs(first, x + 62, y + 17, a, true);
    if (second)
        toast_runs(second, x + 62, y + 17 + line_gap, a, true);
    draw_set_glyph_alpha(1.0f);
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

int app_nav_pad(void)
{
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        if (app.pads[i].connected)
            return i;
    return 0;
}

/* Button presses for menus, with auto-repeat on directions and shoulders. Only the navigating controller counts: the
 * others are for playing. */
static uint32_t nav_pressed(const PadState *pads)
{
    static uint32_t previous;
    static uint64_t repeat_at;
    static int repeats;
    uint32_t held = 0;
    {
        const int i = app_nav_pad();
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

/* The BIOS a region will run, from the choice in Console (core): that file when it is in bios/ (OpenBIOS is built into
 * the core, so it needs no file); with Auto, or a file that is gone, what the core does: the BIOS the folder holds, or
 * OpenBIOS when it holds none. */
static void bios_for_region(const char *key, char *out, size_t size)
{
    for (int i = 0; i < SS_OPT_COUNT; ++i)
    {
        if (strcmp(SS_OPTS[i].key, key) != 0)
            continue;
        const char *file = ss_opt_value(i, app.global.ss_opt[i]); /* "" is Auto */
        char path[SwanStationPS5_PATH_MAX];
        path_join(path, sizeof(path), app.paths.bios, file);
        if (file[0] && access(path, R_OK) == 0)
        {
            str_copy(out, size, file);
            return;
        }
        if (strcmp(file, "openbios.bin") == 0)
        {
            str_copy(out, size, "OpenBIOS");
            return;
        }
        str_copy(out, size, "OpenBIOS");
        DIR *d = opendir(app.paths.bios); /* Auto, or a file that is not there: the core uses what the folder holds */
        if (d)
        {
            struct dirent *e;
            while ((e = readdir(d)))
                if (str_icmp(path_ext(e->d_name), "bin") == 0)
                {
                    str_copy(out, size, e->d_name);
                    break;
                }
            closedir(d);
        }
        return;
    }
    str_copy(out, size, "?");
}

/* Settings > About > BIOS: the one each region will run */
void app_describe_bios(char *out, size_t size)
{
    char j[64], u[64], p[64];
    bios_for_region("swanstation_BIOS_PathNTSCJ", j, sizeof(j));
    bios_for_region("swanstation_BIOS_PathNTSCU", u, sizeof(u));
    bios_for_region("swanstation_BIOS_PathPAL", p, sizeof(p));
    snprintf(out, size, "NTSC-J: %s  \xc2\xb7  NTSC-U: %s  \xc2\xb7  PAL: %s", j, u, p);
}

void app_restart_covers(void)
{
    covers_start(&app.library, &app.paths, &app.global);
    covers_style_loaded = app.global.cover_style;
    covers_download_loaded = app.global.cover_download;
}

static Game dg;                  /* the disc played from the drive, while it is in (see disc_play_frame) */
static Game dp;                  /* a disc put in that is not in the library: its entry on the shelf asks again (see disc_ask_open) */
static bool dp_active, dp_gone;
static bool disc_after_game;
static int dp_removed_mark;
static DiscId dp_id;
static void dp_drop(void);
static bool dg_active, dg_gone;
static int dg_removed_mark;

static bool nfs_rescan_from_mount; /* the rescan that follows a mount does not start another */
static bool nfs_notify;            /* say how the mount went (the player asked for it) */

void app_nfs_notify(void)
{
    nfs_notify = true;
}

#define SCAN_ROOTS_MAX (3 + DRIVE_COUNT * 2 * 2 + DRIVE_COUNT + NFS_MAX_SHARES + NTFSFS_MAX_VOLUMES * 2)
#define SCAN_PATH_MAX (NFS_PATH_MAX > SwanStationPS5_PATH_MAX ? NFS_PATH_MAX : SwanStationPS5_PATH_MAX)

/* The scan that follows an NFS mount lists the shares over the network: it runs on its own thread, into a library of its
 * own, while the shelf keeps the games it had. The main loop takes the result when the thread is done. */
static struct
{
    SDL_Thread *thread;
    SDL_atomic_t done;
    Library lib;
    char paths[SCAN_ROOTS_MAX][SCAN_PATH_MAX];
    const char *roots[SCAN_ROOTS_MAX];
    char index[SwanStationPS5_PATH_MAX];
    int count, nfs_first, shared;
    bool sandboxed;
} scan_async;

static int rescan_thread(void *unused)
{
    (void)unused;
    if (scan_async.sandboxed)
    {
        if (!library_load_index(&scan_async.lib, scan_async.index))
            scan_async.lib.count = 0;
        library_scan_append(&scan_async.lib, scan_async.roots + scan_async.nfs_first, scan_async.shared);
    }
    else
    {
        library_scan(&scan_async.lib, scan_async.roots, scan_async.count);
        if (scan_async.lib.count == 0)
            library_load_index(&scan_async.lib, scan_async.index);
    }
    SDL_AtomicSet(&scan_async.done, 1);
    return 0;
}

static void rescan_async_wait(void)
{
    if (!scan_async.thread)
        return;
    if (scan_async.thread != (SDL_Thread *)1)
        SDL_WaitThread(scan_async.thread, NULL);
    scan_async.thread = NULL;
    SDL_AtomicSet(&scan_async.done, 0);
    library_free(&scan_async.lib);
}

static void rescan_async_start(const char *const *roots, int count, const char *index, int nfs_first, int shared)
{
    for (int i = 0; i < count; ++i)
    {
        str_copy(scan_async.paths[i], sizeof(scan_async.paths[i]), roots[i]);
        scan_async.roots[i] = scan_async.paths[i];
    }
    str_copy(scan_async.index, sizeof(scan_async.index), index);
    scan_async.count = count;
    scan_async.nfs_first = nfs_first;
    scan_async.shared = shared;
    scan_async.sandboxed = app.sandboxed;
    SDL_AtomicSet(&scan_async.done, 0);
    scan_async.thread = SDL_CreateThread(rescan_thread, "library-scan", NULL);
    if (!scan_async.thread)
    {
        scan_async.thread = NULL;
        rescan_thread(NULL);
        scan_async.thread = (SDL_Thread *)1;
    }
}

static void rescan_async_poll(void)
{
    if (!scan_async.thread || app.game || !SDL_AtomicGet(&scan_async.done))
        return;
    if (scan_async.thread != (SDL_Thread *)1)
        SDL_WaitThread(scan_async.thread, NULL);
    scan_async.thread = NULL;
    SDL_AtomicSet(&scan_async.done, 0);
    library_free(&app.library);
    app.library = scan_async.lib;
    memset(&scan_async.lib, 0, sizeof(scan_async.lib));
    if (dg_active)
        library_add_game(&app.library, &dg);
    if (dp_active)
        library_add_game(&app.library, &dp);
    shelf_library_changed();
    app_restart_covers();
}

void app_rescan(void)
{
    rescan_async_wait();
    if (app.settings_game)
    {
        /* the games are listed again: the shelf's game may not be the same pointer (or there) any more */
        app_save_settings();
        app.settings_game = NULL;
        app.game_has_own = false;
        app.settings = app.global;
    }
    /* The games folder, then on each drive both <drive>/<name> and <drive>/<name>/games, for our
     * folder name and for the PSXS5 one. */
    static const char *const names[] = {"SwanStationPS5", "PSXS5"};
    static char drive_roots[DRIVE_COUNT * 2 * 2][SwanStationPS5_PATH_MAX];
    static char usb_roots[DRIVE_COUNT][SwanStationPS5_PATH_MAX];
    static char nfs_roots[NFS_MAX_SHARES + NTFSFS_MAX_VOLUMES * 2][NFS_PATH_MAX];
    static char ntfs_points[NTFSFS_MAX_VOLUMES][NTFSFS_PATH_MAX];
    const char *roots[3 + DRIVE_COUNT * 2 * 2 + DRIVE_COUNT + NFS_MAX_SHARES + NTFSFS_MAX_VOLUMES * 2];
    int root_count = 0;
    roots[root_count++] = app.paths.games;
    roots[root_count++] = "/data/PSXS5/games";
    roots[root_count++] = "/data/homebrew/PPSA99248/games";
    int drive_count = 0;
    for (int d = 0; d < DRIVE_COUNT; ++d)
        for (int n = 0; n < 2; ++n)
        {
            if (d < DRIVE_EXT_COUNT && !drive_usable(d))
                continue;
            char *plain = drive_roots[drive_count++], *games = drive_roots[drive_count++];
            snprintf(plain, SwanStationPS5_PATH_MAX, "%s/%s", drive_paths[d], names[n]);
            snprintf(games, SwanStationPS5_PATH_MAX, "%s/%s/games", drive_paths[d], names[n]);
            roots[root_count++] = plain;
            roots[root_count++] = games;
        }
    /* Settings > System > SwanStationPS5 > Games folder on drives: that name on every drive at once, as the drive's
     * number changes with the ones plugged in */
    if (app.global.usb_folder[0])
        for (int u = 0; u < DRIVE_COUNT; ++u)
        {
            if (u < DRIVE_EXT_COUNT && !drive_usable(u))
                continue;
            snprintf(usb_roots[u], SwanStationPS5_PATH_MAX, "%s/%s", drive_paths[u], app.global.usb_folder);
            roots[root_count++] = usb_roots[u];
        }
    /* Settings > System > NFS shares: the ones that are not mounted are tried on their own thread, so a rescan never
     * waits for a server; the library is scanned again (in the main loop) when that is over */
    if (app.global.nfs_shares[0] && !app.game && !nfs_rescan_from_mount)
        nfs_mount_begin(8000);
    const int nfs_first = root_count;
    const int nfs_count = nfs_rescan_from_mount || app.game ? nfs_mount_points(nfs_roots, NFS_MAX_SHARES) : 0;
    int shared_count = nfs_count;
    if (app.global.ntfs_enabled)
    {
        if (!app.game)
            ntfsfs_scan();
        const int volumes = ntfsfs_mount_points(ntfs_points, NTFSFS_MAX_VOLUMES);
        for (int n = 0; n < volumes; ++n)
        {
            if (app.global.usb_folder[0])
                snprintf(nfs_roots[shared_count++], NFS_PATH_MAX, "%s/%s", ntfs_points[n], app.global.usb_folder);
            snprintf(nfs_roots[shared_count++], NFS_PATH_MAX, "%s/SwanStationPS5/games", ntfs_points[n]);
        }
    }
    for (int n = 0; n < shared_count; ++n)
        roots[root_count++] = nfs_roots[n];
    char index[SwanStationPS5_PATH_MAX];
    path_join(index, sizeof(index), app.paths.root, "library.txt");
    char serials[SwanStationPS5_PATH_MAX];
    path_join(serials, sizeof(serials), app.paths.cache, "serials.tsv");
    library_serial_cache_load(serials);
    if (nfs_rescan_from_mount && !app.game)
    {
        rescan_async_start(roots, root_count, index, nfs_first, shared_count);
        return;
    }
    if (app.sandboxed)
    {
        /* Can't list folders: the sync tool's index is the library. */
        if (!library_load_index(&app.library, index))
            app.library.count = 0;
        library_scan_append(&app.library, roots + nfs_first, shared_count); /* the shares are listed by the app itself */
    }
    else
    {
        library_scan(&app.library, roots, root_count);
        if (app.library.count == 0)
            library_load_index(&app.library, index);
    }
    if (dg_active)
        library_add_game(&app.library, &dg);
    if (dp_active)
        library_add_game(&app.library, &dp);
    if (nfs_rescan_from_mount)
        shelf_library_changed(); /* the shares came up on their own: the player stays where they were */
    else
        shelf_show_all();
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
    const Game *own_game = app.game ? app.game : app.settings_game;
    if (own_game && app.game_has_own)
    {
        char path[SwanStationPS5_PATH_MAX], dir[SwanStationPS5_PATH_MAX];
        path_join(dir, sizeof(dir), app.paths.user, "game-settings");
        make_dirs(dir);
        app_game_config_path(path, sizeof(path), own_game);
        Settings own = app.settings;
        for (int i = 0; i < SS_OPT_COUNT; ++i)
            if (ss_opt_is_bios(i))
                own.ss_opt[i] = SS_OPTS[i].def; /* there is no BIOS per game: the file does not carry it */
        config_save(&own, path);
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

/* A factory reset also drops every game's own settings. */
static void remove_game_settings(void)
{
    char dir[SwanStationPS5_PATH_MAX], file[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.user, "game-settings");
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
        {
            path_join(file, sizeof(file), dir, e->d_name);
            remove(file);
        }
    closedir(d);
}

static void reset_to_factory(void)
{
    remove_game_settings();
    Settings d;
    config_defaults(&d);
    d.last_game = app.global.last_game; /* only where the shelf is stays; the rest, language included, is the factory's */
    app.global = d;
    app.settings = d;
    i18n_set(d.language);
    theme_apply(d.theme);
    sfx_configure(d.ui_sound, (d.ui_volume + 1) * 25);
    remote_update(d.remote);
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

static bool opened_by_daemon;  /* the disc daemon started the app (payload/discd.c leaves <root>/daemon-launch) */
/* A home-screen forwarder (the PS5 Forwarder Format) starts the app with --rom <path> and maybe --exit-after-game:
 * that game starts at once, and with the flag the app closes when it ends. */
static char launch_rom[SwanStationPS5_PATH_MAX];
static bool exit_after_game;
static bool started_from_argument;
static uint64_t app_started_us;

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

/* Splits `s` into lines no wider than max_w, at its own line breaks and at spaces (or between characters when a word is wider, as in Japanese).
 * Returns how many lines were filled; what does not fit in max_lines is dropped. */
#define WRAP_LINE 200
static int wrap_lines(const char *s, float size, int weight, float max_w, char out[][WRAP_LINE], int max_lines)
{
    int n = 0, len = 0, space = -1;
    char line[WRAP_LINE];
    line[0] = '\0';
    for (const char *p = s; *p && n < max_lines;)
    {
        const unsigned char b = (unsigned char)*p;
        if (b == '\n')
        {
            memcpy(out[n++], line, (size_t)len + 1); /* a break the text asks for */
            len = 0;
            line[0] = '\0';
            space = -1;
            ++p;
            continue;
        }
        int c = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        if (len + c >= WRAP_LINE)
            break;
        memcpy(line + len, p, (size_t)c);
        len += c;
        line[len] = '\0';
        if (b == ' ')
            space = len - 1;
        if (len > c && text_width(size, weight, line) > max_w)
        {
            int cut = space > 0 ? space : len - c;
            int rest = space > 0 ? space + 1 : len - c;
            memcpy(out[n], line, (size_t)cut);
            out[n++][cut] = '\0';
            memmove(line, line + rest, (size_t)(len - rest) + 1);
            len -= rest;
            space = -1;
            for (int i = 0; i < len; ++i)
                if (line[i] == ' ')
                    space = i;
        }
        p += c;
    }
    if (len > 0 && n < max_lines)
    {
        memcpy(out[n], line, (size_t)len + 1);
        ++n;
    }
    return n;
}

static void draw_achievement(void)
{
    const bool nostalgia = app.game && app.global.nostalgia && app.screen == SCREEN_GAME;
    if (app.screen == SCREEN_GAME && app.global.ra_tracker && !nostalgia)
        draw_tracker();
    static char title[96], detail[192];
    static uint64_t shown_at;
    const uint64_t length = 4500000;
    static bool showing, quiet;
    uint64_t now = plat_ticks_us();
    if (!showing || now > shown_at + length)
    {
        showing = ra_next_message(title, sizeof(title), detail, sizeof(detail), &quiet);
        if (!showing)
            return;
        /* pop-ups off: messages that arrive during play are dropped, not shown */
        if ((!app.global.ra_popups || nostalgia) && app.screen == SCREEN_GAME)
        {
            showing = false;
            return;
        }
        shown_at = now;
        /* quiet: the summary shown when a game opens; the sound is for the rest */
        if (!quiet)
            sfx_play(SFX_SELECT);
    }
    float t = (float)(now - shown_at) / 1e6f, total = length / 1e6f;
    float in = t < 0.25f ? t / 0.25f : 1.0f, out = total - t < 0.4f ? (total - t) / 0.4f : 1.0f;
    float a = in < out ? in : out, slide = (1.0f - in) * (1.0f - in) * 80.0f;
    if (app.global.ra_popup_style == 1)
    {
        /* compact: a line at the top, two when it does not fit */
        char line[300], rows[2][WRAP_LINE];
        snprintf(line, sizeof(line), "%s  \xc2\xb7  %s", title, detail);
        const float room = plat_width() - 200 - 80;
        int n = wrap_lines(line, 22, FONT_REGULAR, room, rows, 2);
        float lw = text_width(22, FONT_REGULAR, line) + 90;
        if (n > 1 || lw > plat_width() - 200)
            lw = plat_width() - 200;
        float lh = 52 + (n > 1 ? (n - 1) * 28 : 0);
        float lx = (plat_width() - lw) * 0.5f, ly = 24 - slide * 0.5f;
        draw_rrect(lx, ly, lw, lh, n > 1 ? TH_RADIUS_SMALL : 26, argb_alpha(TH_CARD_A(0xf0), a));
        icon_draw(ICON_TROPHY, lx + 18, ly + 11, 30, argb_alpha(TH_GOLD, a));
        for (int i = 0; i < n; ++i)
            text_draw_fit(lx + 60, ly + 13 + i * 28, 22, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, lw - 80,
                          rows[i]);
        return;
    }
    if (app.global.ra_popup_style == 2)
    {
        /* big trophy, in the middle */
        char rows[3][WRAP_LINE];
        int n = wrap_lines(detail, 22, FONT_REGULAR, 700, rows, 3);
        float bw = 760, bh = 300 + (n > 1 ? (n - 1) * 28 : 0), bx = (plat_width() - bw) * 0.5f,
              by = (plat_height() - bh) * 0.5f - 40 + slide * 0.4f;
        draw_rrect(bx, by, bw, bh, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
        draw_circle(bx + bw * 0.5f, by + 92, 62, argb_alpha(0xff2a2410u, a));
        icon_draw(ICON_TROPHY, bx + bw * 0.5f - 44, by + 48, 88, argb_alpha(TH_GOLD, a));
        text_draw_fit(bx + bw * 0.5f, by + 176, 34, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, bw - 60, title);
        for (int i = 0; i < n; ++i)
            text_draw_fit(bx + bw * 0.5f, by + 230 + i * 28, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER,
                          bw - 60, rows[i]);
        return;
    }
    char rows[3][WRAP_LINE];
    int n = wrap_lines(detail, 22, FONT_REGULAR, 660 - 140, rows, 3);
    const float w = 660, h = 116 + (n > 1 ? (n - 1) * 28 : 0), x = plat_width() - w - 48 + slide, y = 48;
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
    draw_rrect(x + 18, y + 18, 80, 80, TH_RADIUS_SMALL, argb_alpha(0xff2a2410u, a));
    icon_draw(ICON_TROPHY, x + 30, y + 30, 56, argb_alpha(TH_GOLD, a));
    text_draw_fit(x + 120, y + 20, 30, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, w - 140, title);
    for (int i = 0; i < n; ++i)
        text_draw_fit(x + 120, y + 66 + i * 28, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_LEFT, w - 140,
                      rows[i]);
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
    if (bezel_shown(&view))
        view.border = 0; /* the bezel's window is the full-height 4:3 picture */

    /* around the picture: black, the current background, or a TV */
    int gx, gy, gw, gh;
    plat_game_rect(&gx, &gy, &gw, &gh);
    if (view.border && !bezel_shown(&view) && gw > 0 && gw < plat_width() - 8)
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
        plat_set_framegen(app.settings.framegen == 2 ? 3 : app.settings.framegen, core_hz, speed, !speed_changed);
    }
    plat_draw_game(&view, play_widescreen_active() ? 16.0f / 9.0f : host_aspect(), dim);
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

/* Also support the BIOS folders of PSXS5 and of the other homebrew (PPSA99248): used when ours has no BIOS file and one of
 * those has. */
static void bios_use_legacy_dir(void)
{
    static const char *const legacy[] = {"/data/PSXS5/bios", "/data/homebrew/PPSA99248/bios"};
    if (bios_dir_has_bin(app.paths.bios))
        return;
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i)
    {
        DIR *d = opendir(legacy[i]);
        if (!d) /* missing or can't be listed: nothing proves it holds a BIOS */
            continue;
        closedir(d);
        if (!bios_dir_has_bin(legacy[i]))
            continue;
        str_copy(app.paths.bios, sizeof(app.paths.bios), legacy[i]);
        SwanStationPS5_log("bios: none in our bios folder, using %s", legacy[i]);
        return;
    }
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

/* The disc import (Settings > System > SwanStationPS5 > Import disc, or a disc put in the drive on the shelf): while it
 * runs the buttons stay out of every screen and nothing can cancel it; when it ends the result is shown and the
 * library looks for the new game */
static char handled_discs[16][24]; /* discs already dealt with since the app opened: each one starts once */
static int handled_count;
static bool auto_launch;           /* the import was started by a disc being put in: start the game when it is done */

static bool disc_handled(const char *key)
{
    for (int i = 0; i < handled_count; ++i)
        if (!strcmp(handled_discs[i], key))
            return true;
    return false;
}

static void disc_mark_handled(const char *key)
{
    if (key[0] && !disc_handled(key) && handled_count < 16)
        str_copy(handled_discs[handled_count++], sizeof(handled_discs[0]), key);
}

/* One of the files in the game's folder carries the disc's "[key]": a disc of a game with several, installed beside
 * the others */
static bool folder_has_disc(const Game *g, const char *tag)
{
    DIR *d = opendir(g->folder);
    bool found = false;
    struct dirent *e;
    while (d && !found && (e = readdir(d)))
        if (strstr(e->d_name, tag))
        {
            const char *ext = path_ext(e->d_name);
            found = !str_icmp(ext, "cue") || !str_icmp(ext, "chd") || !str_icmp(ext, "iso") || !str_icmp(ext, "pbp") ||
                    !str_icmp(ext, "ecm") || !str_icmp(ext, "mds");
        }
    if (d)
        closedir(d);
    return found;
}

/* The game for a disc key in the library: its serial, the "[key]" the installation put in the folder's or the file's
 * name, or a disc of it that was installed beside the first */
static int find_game_by_key(const char *key)
{
    char tag[32];
    snprintf(tag, sizeof(tag), "[%s]", key);
    for (int i = 0; i < app.library.count; ++i)
    {
        const Game *g = &app.library.games[i];
        if ((g->serial[0] && !strcasecmp(g->serial, key)) || strstr(g->path, tag))
            return i;
    }
    for (int i = 0; i < app.library.count; ++i)
        if (folder_has_disc(&app.library.games[i], tag))
            return i;
    return -1;
}

/* Which disc of the game's playlist (.m3u) is the one put in: 0 for the first. -1 when it is not a playlist. */
static int disc_index_in_playlist(const Game *g, const char *key)
{
    if (str_icmp(path_ext(g->path), "m3u") != 0)
        return -1;
    FILE *f = fopen(g->path, "r");
    if (!f)
        return -1;
    char tag[32], line[SwanStationPS5_PATH_MAX];
    snprintf(tag, sizeof(tag), "[%s]", key);
    int index = 0, found = -1;
    while (found < 0 && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#')
            continue;
        if (strstr(line, tag))
            found = index;
        ++index;
    }
    fclose(f);
    return found;
}

/* The first game started in a session the disc daemon opened goes straight in: no animation, no click. Anything after
 * that, or in a session the player opened, plays as usual. */
static bool daemon_direct_boot(void)
{
    static bool used;
    if (used || !opened_by_daemon || plat_ticks_us() - app_started_us > 90000000u)
        return false;
    used = true;
    return true;
}

/* Starts the game on the disc that was put in */
static void play_from_disc(int game, const char *key)
{
    host_set_initial_disc(disc_index_in_playlist(&app.library.games[game], key));
    if (!shelf_play_game(game, daemon_direct_boot()))
        SwanStationPS5_log("disc: the game could not be started from the shelf");
}

/* what went wrong, in a box that stays until X */
static const char *import_why(DiscImportState s)
{
    static const char *const WHY[] = {
        [DI_NO_DRIVE] = "No CD drive was found. Plug the USB CD drive into the console and try again.",
        [DI_NO_DISC] = "The CD drive has no disc ready. Put in a disc, wait for it to spin up and try again.",
        [DI_NOT_PS1] = "This disc is not a PS1 game: its first track is not data or it has no readable volume. Nothing was installed.",
        [DI_UNREADABLE] = "The disc has too many sectors that cannot be read, so the installation was stopped and what was copied was deleted. Clean the disc or try another copy.",
        [DI_DRIVE_LOST] = "The disc or the drive was removed during the installation. What was copied was deleted.",
        [DI_WRITE_FAILED] = "The game could not be written to the games folder. Check that there is free space and try again.",
        [DI_STALLED] = "The installation was stopped: the CD drive did not read anything for 30 seconds. The disc may be dirty, scratched or badly burned. Clean it and try again; what was copied was deleted.",
    };
    return WHY[s];
}

/* Play from disc (the answer to a disc put in): the disc in the drive is read into memory and its game is on the
 * shelf, started by itself, for as long as the disc is in. It is a Game of its own (not in any folder): the library
 * gets it back after a rescan, and it goes when the disc is taken out (or, if it is being played, when that ends). */
static const char *play_why(DiscImportState s)
{
    switch (s)
    {
    case DI_DRIVE_LOST: return "The disc or the drive was removed before the game could be read.";
    case DI_WRITE_FAILED: return "There is not enough memory to read the disc.";
    default: return import_why(s);
    }
}

static void disc_game_id(const DiscId *id, char *out, size_t size)
{
    if (id->serial[0])
    {
        str_copy(out, size, id->serial);
        return;
    }
    size_t w = 0;
    for (const char *p = id->title; *p && w + 1 < size; ++p)
        out[w++] = isalnum((unsigned char)*p) ? *p : '_';
    out[w] = '\0';
}

static void dg_make(void)
{
    const DiscId *id = discplay_id();
    memset(&dg, 0, sizeof(dg));
    str_copy(dg.title, sizeof(dg.title), id->title);
    str_copy(dg.serial, sizeof(dg.serial), id->serial);
    str_copy(dg.path, sizeof(dg.path), discplay_path());
    str_copy(dg.folder, sizeof(dg.folder), "/discplay");
    const char *base = strrchr(dg.path, '/');
    str_copy(dg.disc_name, sizeof(dg.disc_name), base ? base + 1 : dg.path);
    char *dot = strrchr(dg.disc_name, '.');
    if (dot)
        *dot = '\0';
    disc_game_id(id, dg.id, sizeof(dg.id));
    dg.discs = 1;
}

static void dg_drop(void)
{
    SwanStationPS5_log("disc: '%s' is gone: its game leaves the library", dg.title);
    library_remove_path(&app.library, dg.path);
    discplay_stop();
    dg_active = dg_gone = false;
    shelf_library_changed();
}

/* Every frame: the disc read for the game is ready (start it), failed (say why) or went away (drop its game) */
static void disc_play_frame(void)
{
    const DiscPlayState st = discplay_state();
    if (st == DP_FAILED)
    {
        const DiscImportState why = discplay_error();
        discplay_stop();
        app_message_box(play_why(why));
        return;
    }
    if (!dg_active && st == DP_READY && app.screen == SCREEN_LIBRARY && !app.game && shelf_idle() && !box_open &&
        !SC.active)
    {
        dg_make();
        char key[24];
        str_copy(key, sizeof(key), discplay_id()->key);
        dp_drop();
        if (!library_add_game(&app.library, &dg))
        {
            discplay_stop();
            return;
        }
        dg_active = true;
        dg_gone = false;
        dg_removed_mark = discwatch_removed();
        app_restart_covers();
        shelf_show_all();
        SwanStationPS5_log("disc: '%s' (%s) is read into memory and starts from the disc", dg.title, key);
        play_from_disc(app.library.count - 1, key);
        return;
    }
    if (!dg_active)
        return;
    if (!dg_gone && (discwatch_removed() != dg_removed_mark || discplay_lost()))
        dg_gone = true;
    if (dg_gone && !app.game && app.screen == SCREEN_LIBRARY && shelf_idle() && !SC.active)
        dg_drop();
}

/* The disc is being identified: the buttons stay out of the screens below, and a panel says so */
static bool play_input(uint32_t *pressed)
{
    if (discplay_state() != DP_PREPARING)
        return false;
    *pressed = 0;
    return true;
}

static void play_draw(void)
{
    if (discplay_state() != DP_PREPARING)
        return;
    const char *msg = tr("Reading the disc...");
    float w = text_width(26, FONT_REGULAR, msg) + 96, h = 84;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - 300;
    draw_rrect(x, y, w, h, TH_RADIUS_SMALL, 0xf0000000u | (TH_PILL & 0xffffffu));
    text_draw(plat_width() * 0.5f, y + 28, 26, FONT_REGULAR, TH_TEXT, ALIGN_CENTER, msg);
}

#define CANCEL_HOLD 2.0f /* seconds O is held to cancel an import, as to close the app */
static float cancel_hold;

static bool import_input(uint32_t *pressed)
{
    DiscImportState s = discimport_state();
    if (s == DI_IDLE)
        return false;
    uint32_t was_pressed = *pressed;
    *pressed = 0;
    if (s == DI_RUNNING)
    {
        discimport_check_stall(); /* no progress for 30 seconds: it is given up and the next frame says why */
        /* O: a tap only says how to cancel; holding it (like closing the app) cancels */
        static bool armed;
        const bool circle = (app.pads[app_nav_pad()].buttons & BIT(BTN_CIRCLE)) != 0;
        if (was_pressed & BIT(BTN_CIRCLE))
        {
            armed = true;
            app_toast("Hold {Circle} to cancel the installation.");
        }
        if (circle && armed)
        {
            cancel_hold += app.dt;
            if (cancel_hold >= CANCEL_HOLD)
            {
                cancel_hold = 0.0f;
                armed = false;
                discimport_cancel();
            }
        }
        else
        {
            cancel_hold = 0.0f;
            armed = false;
        }
        return true;
    }
    cancel_hold = 0.0f;
    char msg[160], key[24];
    str_copy(key, sizeof(key), discimport_key());
    disc_mark_handled(key); /* whatever happened, the same disc does not start it again by itself */
    bool launch = auto_launch;
    auto_launch = false;
    switch (s)
    {
    case DI_OK: /* no notice: the game appears on the shelf, and starts by itself if a disc put in began this */
        msg[0] = '\0';
        dp_drop();
        app_rescan();
        break;
    case DI_OK_INCOMPLETE:
        snprintf(msg, sizeof(msg), tr("Installed with %d unreadable sectors: %s"), discimport_bad(), discimport_title());
        dp_drop();
        app_rescan();
        break;
    case DI_ALREADY:
        snprintf(msg, sizeof(msg), tr("Already in your games: %s"), discimport_title());
        dp_drop();
        app_rescan();
        break;
    default: break;
    }
    if (s == DI_CANCELLED)
    {
        discimport_ack();
        app_toast("Installation cancelled.");
        return true;
    }
    if (s != DI_OK && s != DI_OK_INCOMPLETE && s != DI_ALREADY)
    {
        discimport_ack();
        app_message_box(import_why(s));
        return true;
    }
    discimport_ack();
    if (msg[0])
        app_toast_for(msg, 6.0f);
    if (launch) /* a disc put in on the shelf: the game starts by itself once it is in */
    {
        int game = find_game_by_key(key);
        if (game >= 0)
            play_from_disc(game, key);
    }
    return true;
}

/* The disc daemon (payload/discd.c) and the app share the drive: the app writes the time into <root>/app-alive every
 * couple of seconds, so the daemon leaves the drive to it; and the daemon writes <root>/disc-busy while it installs a
 * disc without the app, so the app leaves it alone and keeps its hands off the half-written files. */
static bool stamp_is_fresh(const char *name, int within)
{
    char path[SwanStationPS5_PATH_MAX];
    path_join(path, sizeof(path), app.paths.root, name);
    FILE *f = fopen(path, "r");
    long long t = 0;
    if (!f)
        return false;
    int got = fscanf(f, "%lld", &t);
    fclose(f);
    long long now = (long long)time(NULL);
    return got == 1 && now >= t && now - t <= within;
}

static bool daemon_installing(void)
{
    return stamp_is_fresh("disc-busy", 10);
}

static void write_alive_stamp(void)
{
    static uint64_t last_us;
    uint64_t now = plat_ticks_us();
    if (last_us && now - last_us < 2000000)
        return;
    last_us = now;
    char path[SwanStationPS5_PATH_MAX];
    path_join(path, sizeof(path), app.paths.root, "app-alive");
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%lld\n", (long long)time(NULL));
        fclose(f);
    }
}

/* A disc put in the drive: one that is not in the library asks how to play it (from the disc, or copied first); one that
 * is there is shown on the shelf and asks whether to start it. Nothing starts by itself. */
static struct
{
    bool open;
    bool known;
    int sel;
    int game;
    int removed_mark;
    float t;
    DiscId id;
} dq;

static int dp_index(void)
{
    if (!dp_active)
        return -1;
    for (int i = 0; i < app.library.count; ++i)
        if (!strcmp(app.library.games[i].path, dp.path))
            return i;
    return -1;
}

static void dp_make(const DiscId *id)
{
    memset(&dp, 0, sizeof(dp));
    str_copy(dp.title, sizeof(dp.title), id->title);
    str_copy(dp.serial, sizeof(dp.serial), id->serial);
    snprintf(dp.path, sizeof(dp.path), "/discpending/%s", id->key);
    str_copy(dp.folder, sizeof(dp.folder), "/discpending");
    str_copy(dp.disc_name, sizeof(dp.disc_name), id->key);
    disc_game_id(id, dp.id, sizeof(dp.id));
    dp.discs = 1;
    dp_id = *id;
}

static void dp_drop(void)
{
    if (!dp_active)
        return;
    library_remove_path(&app.library, dp.path);
    dp_active = dp_gone = false;
    shelf_library_changed();
}

static void disc_ask_close(void)
{
    dq.open = false;
}

static void disc_ask_open(const DiscId *id, int game)
{
    memset(&dq, 0, sizeof(dq));
    dq.open = true;
    dq.known = game >= 0;
    dq.game = game;
    dq.id = *id;
    dq.removed_mark = discwatch_removed();
}

/* X on the entry of a disc that is not in the library: the question again. True when the game was that entry. */
bool app_disc_pending(int index)
{
    if (!dp_active || index < 0 || index >= app.library.count || strcmp(app.library.games[index].path, dp.path))
        return false;
    if (dq.open || discimport_state() != DI_IDLE || discplay_state() != DP_IDLE)
        return true;
    disc_ask_open(&dp_id, -1);
    return true;
}

static bool disc_ask_input(uint32_t *pressed)
{
    if (!dq.open)
        return false;
    if (discwatch_removed() != dq.removed_mark)
    {
        disc_ask_close();
        return false;
    }
    if (!dq.known && (*pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT))))
    {
        dq.sel ^= 1;
        sfx_play(SFX_CLICK);
    }
    else if (*pressed & BIT(BTN_CROSS))
    {
        const DiscId id = dq.id;
        const bool known = dq.known;
        const int sel = dq.sel;
        disc_ask_close();
        if (known)
        {
            int game = find_game_by_key(id.key);
            if (game >= 0)
            {
                host_set_initial_disc(disc_index_in_playlist(&app.library.games[game], id.key));
                if (!shelf_start_game(game))
                    SwanStationPS5_log("disc: the game could not be started from the shelf");
            }
        }
        else if (sel == 0)
        {
            if (!discplay_start())
                SwanStationPS5_log("disc: the disc could not be read for playing");
        }
        else
        {
            char ext_games[SwanStationPS5_PATH_MAX];
            auto_launch = discimport_start(app.global.install_to_ext && drives_ext_games(ext_games, sizeof(ext_games)) ? ext_games : app.paths.games);
        }
    }
    else if (*pressed & BIT(BTN_CIRCLE))
    {
        disc_ask_close();
        sfx_play(SFX_BACK);
    }
    *pressed = 0;
    return true;
}

static void disc_ask_wrap(const char *m, float x, float y, float size, float width, uint32_t color)
{
    char line[256];
    while (*m)
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
            if (best > 0 && text_width(size, FONT_REGULAR, line) > width)
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
        text_draw(x, y, size, FONT_REGULAR, color, ALIGN_CENTER, line);
        y += size * 1.45f;
        m += best;
        while (*m == ' ')
            ++m;
    }
}

static void disc_ask_card(float x, float y, float w, float h, bool sel, const char *title, const char *l1, const char *l2, float a)
{
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(sel ? TH_ROW_SELECTED : TH_PILL, a));
    if (sel)
        draw_rrect_outline(x, y, w, h, TH_RADIUS, 3, argb_alpha(TH_FOCUS, a));
    text_draw_fit(x + w * 0.5f, y + 22, 32, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, w - 32, title);
    text_draw_fit(x + w * 0.5f, y + 78, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, w - 32, l1);
    text_draw_fit(x + w * 0.5f, y + 78 + 34, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER, w - 32, l2);
}

static void disc_ask_draw(void)
{
    if (!dq.open)
        return;
    dq.t = fminf(dq.t + app.dt * 8.0f, 1.0f);
    const float a = dq.t, w = 940, h = dq.known ? 540 : 640, pad = 48, cs = 190;
    float x = (plat_width() - w) * 0.5f, y = (plat_height() - h) * 0.5f + (1.0f - a) * 24.0f;
    draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xa0000000u, a));
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_PILL_A(0xf8), a));

    PlatTexture *cover = NULL;
    const int cover_game = dq.known ? dq.game : dp_index();
    if (cover_game >= 0 && cover_game < app.library.count)
        cover = shelf_cover_or_none(cover_game);
    else
        cover = shelf_no_cover();
    draw_rrect(x + pad, y + pad, cs, cs, TH_RADIUS_SMALL, argb_alpha(TH_DIVIDER, a));
    if (cover)
    {
        int tw, th;
        plat_texture_size(cover, &tw, &th);
        float k = fminf(cs / (float)tw, cs / (float)th), dw = tw * k, dh = th * k;
        plat_draw_texture(cover, x + pad + (cs - dw) * 0.5f, y + pad + (cs - dh) * 0.5f, dw, dh, argb_alpha(0xffffffffu, a), true);
    }

    const char *drive = tr("Disc in the drive");
    const float ds = 22, is = 36;
    float dw = text_width(ds, FONT_REGULAR, drive);
    icon_draw(ICON_DISC, x + w - pad - dw - is - 10, y + pad - 4, is, argb_alpha(TH_FOCUS, a));
    text_draw(x + w - pad, y + pad + 2, ds, FONT_REGULAR, argb_alpha(TH_HINT, a), ALIGN_RIGHT, drive);

    const float tx = x + pad + cs + 32, tw = x + w - pad - tx;
    text_draw_fit(tx, y + pad + 62, 38, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, tw, dq.id.title);
    if (dq.id.serial[0])
        text_draw(tx, y + pad + 118, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_LEFT, dq.id.serial);

    float yy = y + pad + cs + 40;
    const float hs = 28, gap = hs * 0.4f;
    if (dq.known)
    {
        disc_ask_wrap(tr("This game is already in your games. Do you want to start it?"), x + w * 0.5f, yy, 30, w - 2 * pad,
                      argb_alpha(TH_TEXT, a));
        const char *yes = tr("Start"), *no = tr("Cancel");
        const float gw1 = pad_glyph_width(GLYPH_CROSS, hs), gw2 = pad_glyph_width(GLYPH_CIRCLE, hs);
        const float w1 = gw1 + gap + text_width(hs, FONT_BOLD, yes), w2 = gw2 + gap + text_width(hs, FONT_REGULAR, no);
        const float space = 56, hy = y + h - pad - hs;
        float hx = x + (w - (w1 + space + w2)) * 0.5f;
        draw_pad_glyph(GLYPH_CROSS, hx + gw1 * 0.5f, hy + hs * 0.6f, hs);
        text_draw(hx + gw1 + gap, hy, hs, FONT_BOLD, argb_alpha(TH_FOCUS, a), ALIGN_LEFT, yes);
        hx += w1 + space;
        draw_pad_glyph(GLYPH_CIRCLE, hx + gw2 * 0.5f, hy + hs * 0.6f, hs);
        text_draw(hx + gw2 + gap, hy, hs, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, no);
        return;
    }
    text_draw(x + w * 0.5f, yy, 28, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_CENTER, tr("How do you want to play it?"));
    const float cg = 24, cw = (w - 2 * pad - cg) * 0.5f, ch = 150, cy = yy + 46;
    disc_ask_card(x + pad, cy, cw, ch, dq.sel == 0, tr("Play from disc"), tr("Starts right now."), tr("The disc stays in the drive."), a);
    disc_ask_card(x + pad + cw + cg, cy, cw, ch, dq.sel == 1, tr("Copy to storage"), tr("A few minutes, then it plays."),
                  tr("After that the disc can come out."), a);
    const float fs = 24, hy = y + h - pad - fs;
    const int dirs[2] = {GLYPH_LEFT, GLYPH_RIGHT};
    const char *l1 = tr("Choose"), *l2 = tr("Select"), *l3 = tr("Cancel");
    const float w1 = hint_combo_width(dirs, 2, l1, fs), w2 = pad_glyph_width(GLYPH_CROSS, fs) + fs * 0.35f + text_width(fs, FONT_REGULAR, l2),
                w3 = pad_glyph_width(GLYPH_CIRCLE, fs) + fs * 0.35f + text_width(fs, FONT_REGULAR, l3), sp = 40;
    float hx = x + (w - (w1 + w2 + w3 + 2 * sp)) * 0.5f;
    hx += draw_hint_combo(hx, hy, dirs, 2, '/', l1, fs, argb_alpha(TH_HINT, a)) + sp;
    hx += draw_hint(hx, hy, GLYPH_CROSS, l2, fs, argb_alpha(TH_HINT, a)) + sp;
    draw_hint(hx, hy, GLYPH_CIRCLE, l3, fs, argb_alpha(TH_HINT, a));
}

/* Every frame: on the shelf, a disc newly put in the USB CD drive asks what to do with it */
static void disc_auto_frame(void)
{
    write_alive_stamp();
    if (dp_active && !dp_gone && discwatch_removed() != dp_removed_mark)
        dp_gone = true;
    if (dp_gone && !dq.open && !app.game && app.screen == SCREEN_LIBRARY && shelf_idle() && !SC.active)
        dp_drop();
    disc_play_frame();
    const bool daemon_busy = daemon_installing();
    discwatch_pause(daemon_busy); /* the daemon is at the drive */
    if (daemon_busy)
        return;
    /* a disc taken out forgets that it was handled: put back, it asks again. One left in does not ask twice. */
    static int removed_seen;
    if (discwatch_removed() != removed_seen)
    {
        removed_seen = discwatch_removed();
        handled_count = 0;
    }
    bool free_for_disc = app.screen == SCREEN_LIBRARY && !app.game && shelf_idle() && !dq.open &&
                         discimport_state() == DI_IDLE && discplay_state() == DP_IDLE && !box_open && !SC.active;
    discwatch_set_active(free_for_disc);
    DiscId id;
    if (!free_for_disc || !discwatch_take(&id) || disc_handled(id.key))
        return;
    disc_mark_handled(id.key);
    int game = find_game_by_key(id.key);
    const bool after = disc_after_game && plat_ticks_us() - app_started_us < 15000000u;
    const bool ask = app.global.disc_ask && !after;
    SwanStationPS5_log("disc: '%s' (%s) put in: %s", id.title, id.key, game >= 0 ? "it is in the library, asking whether to start it" : "asking how to play it");
    if (game >= 0)
        shelf_show_game(game);
    else
    {
        dp_drop();
        dp_make(&id);
        if (library_add_game(&app.library, &dp))
        {
            dp_active = true;
            dp_gone = false;
            dp_removed_mark = discwatch_removed();
            app_restart_covers();
            shelf_show_all();
            shelf_show_game(dp_index());
        }
    }
    disc_after_game = false;
    if (ask)
        disc_ask_open(&id, game);
}

static void import_draw(void)
{
    static Eta eta;
    static int eta_next;
    static long eta_left_s;
    static double eta_at;
    const double now = SDL_GetTicks() / 1000.0;
    if (discimport_state() != DI_RUNNING)
    {
        eta = (Eta){0};
        eta_next = 0;
        eta_left_s = -1;
        return;
    }
    const char *title = discimport_title();
    char msg[200];
    if (title[0])
        snprintf(msg, sizeof(msg), tr("Installing %s..."), title);
    else
        str_copy(msg, sizeof(msg), tr("Installing..."));
    PlatTexture *cover = (PlatTexture *)discimport_cover();
    if (!cover && discimport_cover_none())
        cover = (PlatTexture *)shelf_no_cover(); /* as on the shelf: assets/nocover2.png */
    /* a fixed size, so the card does not change while the disc is identified and the cover arrives */
    const float ch = 200, cw = ch, text_w = 760;
    float w = text_w + cw + 32 + 64, h = ch + 64;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - h - 140;
    float p = discimport_permille() / 1000.0f;
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", discimport_permille() / 10);
    if (discimport_dumping() && title[0])
    {
        if (eta_next == 0)
        {
            eta.prev_time = now;
            eta_next = 5;
            eta_left_s = -1;
        }
        else if (discimport_permille() / 10 >= eta_next)
        {
            long l = eta_left(&eta, p, now, discimport_total_sectors());
            if (l >= 0)
            {
                eta_left_s = l;
                eta_at = now;
            }
            while (eta_next <= discimport_permille() / 10)
                eta_next += 5;
        }
    }
    draw_rrect(x, y, w, h, TH_RADIUS_SMALL, 0xf0000000u | (TH_PILL & 0xffffffu));
    draw_rrect(x + 32, y + 32, cw, ch, TH_RADIUS_SMALL, TH_DIVIDER);
    if (cover)
    {
        int tw, th;
        plat_texture_size(cover, &tw, &th);
        float k = fminf(cw / (float)tw, ch / (float)th), dw = tw * k, dh = th * k;
        plat_draw_texture(cover, x + 32 + (cw - dw) * 0.5f, y + 32 + (ch - dh) * 0.5f, dw, dh, 0xffffffffu, true);
    }
    const float tx = x + 32 + cw + 32, tw_area = x + w - 32 - tx;
    text_draw_fit(tx, y + 44, 30, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tw_area, msg);
    float bar_w = tw_area - 90;
    draw_rrect(tx, y + 108, bar_w, 12, 6, TH_DIVIDER);
    draw_rrect(tx, y + 108, fmaxf(12.0f, bar_w * fminf(p, 1.0f)), 12, 6, TH_FOCUS);
    text_draw(tx + tw_area, y + 94, 26, FONT_REGULAR, TH_TEXT, ALIGN_RIGHT, pct);
    if (eta_left_s >= 0)
    {
        long sec = (long)(eta_left_s - (now - eta_at) + 2) / 5 * 5;
        char when[64];
        if (sec < 5)
            sec = 5;
        if (sec >= 60)
            snprintf(when, sizeof(when), tr("%ld min %ld s left"), sec / 60, sec % 60);
        else
            snprintf(when, sizeof(when), tr("%ld s left"), sec);
        text_draw(tx, y + 138, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, when);
    }
    {
        const char *games = discimport_games_dir();
        char where[64], drive[24];
        if (strncmp(games, "/mnt/", 5) == 0)
        {
            str_copy(drive, sizeof(drive), games);
            char *slash = strchr(drive + 5, '/');
            if (slash)
                *slash = '\0';
        }
        else
            str_copy(drive, sizeof(drive), "/data");
        snprintf(where, sizeof(where), tr("Installing to: %s"), drive);
        text_draw(tx, y + 172, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, where);
    }
    /* bottom right: how to cancel */
    const char *cancel = tr("Cancel (hold)");
    const float cancel_w = text_width(22, FONT_REGULAR, cancel);
    text_draw(x + w - 32, y + h - 58, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, cancel);
    draw_pad_glyph(GLYPH_CIRCLE, x + w - 32 - cancel_w - 26, y + h - 46, 28);
    if (cancel_hold > 0.3f)
    {
        /* O held: a bar fills up to the cancel, as when closing the app */
        const char *cmsg = tr("Keep holding to cancel the installation...");
        float cwid = text_width(26, FONT_REGULAR, cmsg) + 96, chgt = 84;
        float cx = (plat_width() - cwid) * 0.5f, cy = y - chgt - 24;
        draw_rrect(cx, cy, cwid, chgt, TH_RADIUS_SMALL, 0xf0000000u | (TH_PILL & 0xffffffu));
        text_draw(plat_width() * 0.5f, cy + 12, 26, FONT_REGULAR, TH_TEXT, ALIGN_CENTER, cmsg);
        draw_rrect(cx + 32, cy + 56, cwid - 64, 12, 6, TH_DIVIDER);
        draw_rrect(cx + 32, cy + 56, fmaxf(12.0f, (cwid - 64) * fminf(cancel_hold / CANCEL_HOLD, 1.0f)), 12, 6,
                   TH_DANGER);
    }
}

static void crop_ratio_text(char *out, size_t size, float r)
{
    static const struct
    {
        int w, h;
    } COMMON[] = {{4, 3}, {16, 9}, {3, 2}, {1, 1}, {5, 4}, {8, 7}, {2, 1}, {21, 9}, {10, 7}, {16, 10}, {19, 9}};
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); ++i)
        if (fabsf(r * COMMON[i].h / COMMON[i].w - 1.0f) < 0.01f)
        {
            snprintf(out, size, "%d:%d", COMMON[i].w, COMMON[i].h);
            return;
        }
    snprintf(out, size, "%.2f:1", r);
}

static const char *menu_combo_hint(void)
{
    if (app.global.menu_hold == 0)
        return app.global.menu_combo == 1   ? "Press {L3} + {R3} to open the game menu."
               : app.global.menu_combo == 2 ? "Press {Options} + {Touchpad} to open the game menu."
                                            : "Press {Touchpad} + {R3} to open the game menu.";
    return app.global.menu_combo == 1   ? "Hold {L3} + {R3} to open the game menu."
           : app.global.menu_combo == 2 ? "Hold {Options} + {Touchpad} to open the game menu."
                                        : "Hold {Touchpad} + {R3} to open the game menu.";
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
        host_set_fixes(fixes, info && app.settings.game_fixes ? info : NULL);
        if (fixes || (info && app.settings.game_fixes && info->set))
            SwanStationPS5_log("start: known fixes %x, values %x (game database)", fixes, info ? info->set : 0u);
    }
    SwanStationPS5_log("start: %s (%s) from %s%s", shelf_game_title(g), g->serial, g->path,
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
    {
        char hint[200];
        snprintf(hint, sizeof(hint), "%s", tr(menu_combo_hint()));
        /* a DualShock: how to press its ANALOG button (the core's combo, Settings > Controls) */
        if (app.settings.analog && !controls_special_for(g, &app.settings) && !controls_gun_active())
        {
            static const char *const COMBOS[] = {
                "{L1} + {R1} + {L3} + {R3}", "{L1} + {L2} + {R1} + {R2} + {Options} + {Touchpad}",
                "{L1} + {R1} + {Touchpad}", "{L1} + {R1} + {Options}", "{L1} + {R1} + {L3}", "{L1} + {R1} + {R3}",
                "{L2} + {R2} + {Touchpad}", "{L2} + {R2} + {Options}", "{L2} + {R2} + {L3}", "{L2} + {R2} + {R3}",
                "{L3} + {R3}"};
            int combo = 2; /* L1 + R1 + Select, the core's own default */
            for (int i = 0; i < SS_OPT_COUNT; ++i)
                if (!strcmp(SS_OPTS[i].key, "swanstation_Controller_AnalogCombo"))
                {
                    int v = app.settings.ss_opt[i];
                    combo = v >= 0 && v < 11 ? v : SS_OPTS[i].def;
                }
            size_t n = strlen(hint);
            snprintf(hint + n, sizeof(hint) - n, "\n");
            n = strlen(hint);
            snprintf(hint + n, sizeof(hint) - n, tr("%s for the ANALOG button."), COMBOS[combo]);
        }
        app_toast_for(hint, 8.0f);
    }
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
    if (resume)
        play_load_resume();
    if (controls_gun_active())
        app_toast("Light gun: point the controller at the screen, {R2} fires, {R3} re-centres");
    app.screen = SCREEN_GAME;
}

void app_start_bios(void)
{
    static Game bios;
    memset(&bios, 0, sizeof(bios));
    str_copy(bios.title, sizeof(bios.title), "BIOS");
    str_copy(bios.id, sizeof(bios.id), "bios");
    app_close_shelf_settings(); /* the settings in effect are the global ones, not those of the game under the cursor */
    char error[480];
    app.game_has_own = false;
    app.settings = app.global;
    play_rewind_reset();
    controls_start(&bios, &app.settings);
    app.play_seconds = 0;
    host_set_gun(0);
    host_set_special(0);
    host_set_fixes(0, NULL);
    static const char *const boot_regions[] = {"NTSC-U", "NTSC-J", "PAL"};
    const char *boot_region = boot_regions[app.global.bios_start >= 0 && app.global.bios_start < 3 ? app.global.bios_start : 0];
    host_set_region_override(boot_region); /* the BIOS the console's choice for that region names */
    SwanStationPS5_log("start: BIOS with no disc (%s)", boot_region);
    if (!host_load("", "", &app.paths, &app.settings, error, sizeof(error)))
    {
        host_set_region_override(NULL);
        controls_stop();
        app_toast(error);
        return;
    }
    app.game = &bios;
    app.game_index = -1;
    if (!bios_file_present())
        app_toast_for("BIOS not found, SCPH1001.BIN is recommended due to the low compatibility of OpenBios.", 10.0f);
    plat_audio_open(host_sample_rate());
    plat_audio_clear();
    cheats_clear(&app.cheats);
    app_toast_for(menu_combo_hint(), 8.0f);
    app.screen = SCREEN_GAME;
}

/* A game leaves memory behind that the heap cannot give back (a console that rests with it mapped does not wake up), so
 * going back to the library starts the title again: the system ends this process and runs the title's executable. */
static void restart_for_clean_memory(void)
{
#if defined(__PROSPERO__)
    static bool asked;
    if (asked)
        return;
    asked = true;
    app_save_settings();
    stats_save();
    char path[SwanStationPS5_PATH_MAX];
    path_join(path, sizeof(path), app.paths.root, "open-library");
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%lld\n", (long long)time(NULL));
        fclose(f);
    }
    SwanStationPS5_log("restart: the title starts again to give the game's memory back");
    extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
    int rc = sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
    SwanStationPS5_log("restart: refused (0x%x), staying in this process", (unsigned)rc);
    remove(path);
    asked = false;
#endif
}

static void disc_after_stop(void)
{
    if (!dg_active)
        return;
    const bool in_drive = !dg_gone && discwatch_removed() == dg_removed_mark && !discplay_lost();
    DiscId id = *discplay_id();
    library_remove_path(&app.library, dg.path);
    discplay_stop();
    dg_active = dg_gone = false;
    if (in_drive)
    {
        dp_drop();
        dp_make(&id);
        if (library_add_game(&app.library, &dp))
        {
            dp_active = true;
            dp_gone = false;
            dp_removed_mark = discwatch_removed();
        }
    }
    app_restart_covers();
    shelf_show_all();
    if (dp_active)
        shelf_show_game(dp_index());
}

void app_stop_game(void)
{
    host_set_region_override(NULL);
    play_save_resume(false);
    play_rewind_reset();
    int unlocked, total;
    GameStats *st = app.game && !game_is_bios(app.game) ? stats_get(app.game->id) : NULL;
    if (st && ra_game_progress(&unlocked, &total))
    {
        st->ach_unlocked = unlocked;
        st->ach_total = total;
    }
    stats_save();
    if (!game_is_bios(app.game))
        ra_game_unloaded();
    controls_stop();
    host_unload();
    plat_audio_open(UI_RATE);
    plat_audio_clear();
    cheats_clear(&app.cheats);
    app.game = NULL;
    sfx_silence(false);
    app.game_has_own = false;
    app.settings = app.global;
    app.screen = SCREEN_LIBRARY;
    if (started_from_argument && exit_after_game)
    {
        SwanStationPS5_log("forwarder: the game ended, closing (--exit-after-game)");
        app.quit_requested = true;
    }
    else if (app.global.restart_after_game)
        restart_for_clean_memory();
    else
        disc_after_stop();
}

/* The library game a forwarder's --rom names: that file, else the game in the folder holding it (or in that folder).
 * -1 when none. */
static int game_for_launch_path(const char *path)
{
    for (int i = 0; i < app.library.count; ++i)
        if (strcmp(app.library.games[i].path, path) == 0)
            return i;
    char folder[SwanStationPS5_PATH_MAX];
    str_copy(folder, sizeof(folder), path);
    size_t length = strlen(folder);
    while (length > 1 && folder[length - 1] == '/')
        folder[--length] = '\0';
    for (int i = 0; i < app.library.count; ++i)
        if (strcmp(app.library.games[i].folder, folder) == 0)
            return i;
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder)
    {
        *slash = '\0';
        for (int i = 0; i < app.library.count; ++i)
            if (strcmp(app.library.games[i].folder, folder) == 0 &&
                strcmp(app.library.games[i].folder, app.paths.games) != 0)
                return i;
    }
    return -1;
}

/* Settings from the shelf: the game under the cursor can have its own, as from the game's menu */
void app_open_shelf_settings(const Game *game)
{
    app.settings_game = game;
    if (game)
    {
        char own[SwanStationPS5_PATH_MAX];
        app_game_config_path(own, sizeof(own), game);
        app.game_has_own = config_load_game(&app.settings, &app.global, own);
    }
    app_open_settings(SCREEN_LIBRARY);
}

void app_close_shelf_settings(void)
{
    if (app.game || !app.settings_game)
        return;
    app.settings_game = NULL;
    app.game_has_own = false;
    app.settings = app.global;
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
    {
        uint32_t rgb = app.settings.lightbar == 2 && cover ? cover : players[i];
        if (app.settings.lightbar == 3) /* red in analog mode, off in digital */
            rgb = app.game && host_analog_mode(i) ? 0xff0000 : 0x000000;
        plat_set_lightbar(i, rgb);
    }
}

static float auto_since, auto_shown; /* auto-save: time since the last, and its icon */

/* Buttons still held from the menu when the game comes back (the Cross that
 * chose Resume): the game doesn't see them until they're let go. */
static uint32_t held_from_menu;
static bool input_reported;      /* the first button that reached the game was logged */
static uint64_t input_start_us;  /* when the game screen was entered */

static void game_screen(PadState *pads)
{
    uint32_t held_now = 0;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        held_now |= pads[i].buttons;
    held_from_menu &= held_now;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        pads[i].buttons &= ~held_from_menu;
    if (!input_reported)
    {
        uint32_t reaching = 0;
        for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
            reaching |= pads[i].buttons;
        if (reaching)
        {
            input_reported = true;
            SwanStationPS5_log("input: the first buttons reach the game (0x%x) %.1f s after it began", reaching,
                      (double)(plat_ticks_us() - input_start_us) / 1e6);
        }
    }

    /* Touchpad: a tap is the PS1's Select (the PS5 SDL driver has no Create
     * button); the menu is the touchpad held for 1 second. */
    static uint64_t touch_since[SwanStationPS5_MAX_PADS];
    static bool touch_used[SwanStationPS5_MAX_PADS];
    static int select_frames[SwanStationPS5_MAX_PADS];
    bool touch_of[SwanStationPS5_MAX_PADS], chord_of[SwanStationPS5_MAX_PADS];
    bool touch = false, chord = false;
    uint64_t t_now = plat_ticks_us();
    bool open_menu = false;
    /* touchpad held + R2: fast forward; + L2: rewind. The shortcuts are player 1's; on the other
     * pads the touchpad is just that pad's Select */
    uint32_t all = pads[0].buttons;
    /* touchpad + two shoulder buttons: the DualShock's analog mode combo (L1 + R1 + Select...): the
     * touchpad is the Select held down, and the shoulders go to the game */
    const uint32_t shoulders = BIT(BTN_L1) | BIT(BTN_R1) | BIT(BTN_L2) | BIT(BTN_R2);
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        touch_of[i] = pads[i].buttons & BIT(BTN_MENU);
        chord_of[i] = touch_of[i] && __builtin_popcount(pads[i].buttons & shoulders) >= 2;
    }
    touch = touch_of[0];
    chord = chord_of[0];
    bool fast = touch && !chord && (all & BIT(BTN_R2)), back = touch && !chord && (all & BIT(BTN_L2));
    speed_changed = fast || back;
    if (touch && (fast || back || chord))
        touch_used[0] = true; /* a combo, not Select or the menu */
    /* touchpad + Square: screenshot; + Triangle / Circle: timer */
    static uint32_t combo_all_prev;
    uint32_t newly = touch ? all & ~combo_all_prev : 0;
    combo_all_prev = all;
    const uint32_t combo_keys = BIT(BTN_SQUARE) | BIT(BTN_TRIANGLE) | BIT(BTN_CIRCLE);
    if (touch && !chord && (all & combo_keys))
        touch_used[0] = true;
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
    /* The touchpad is also the shortcuts' key, so Select waits this long to be sure no shortcut
     * follows; after it, holding the touchpad holds Select until a shortcut is used. */
    const uint64_t select_hold_us = 0, tap_us = 150000;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        if (touch_of[i])
        {
            if (!touch_since[i])
                touch_since[i] = t_now;
        }
        else
        {
            /* a quick tap: Select for ~100 ms, long enough for every game to see it (a longer
             * touch has been held as Select while it lasted, below) */
            if (touch_since[i] && !touch_used[i] && t_now - touch_since[i] <= tap_us)
                select_frames[i] = 6;
            touch_since[i] = 0;
            touch_used[i] = false;
        }
    }
    /* player 1's menu combination held for the chosen time: the SwanStationPS5 menu */
    static uint64_t menu_since;
    static const uint64_t MENU_HOLD_US[3] = {0, 500000, 1000000};
    const uint32_t first_pad = pads[0].buttons;
    const int menu_combo = app.global.menu_combo;
    const bool menu_held = menu_combo == 1 ? (first_pad & BIT(BTN_L3)) && (first_pad & BIT(BTN_R3)) && !(first_pad & shoulders)
                           : menu_combo == 2 ? touch_of[0] && !chord_of[0] && (first_pad & BIT(BTN_START))
                                             : touch_of[0] && !chord_of[0] && (first_pad & BIT(BTN_R3));
    if (!menu_held)
        menu_since = 0;
    else if (!menu_since)
        menu_since = t_now;
    if (menu_held && t_now - menu_since >= MENU_HOLD_US[app.global.menu_hold])
    {
        open_menu = true;
        menu_since = 0;
        touch_used[0] = true; /* not a Select when it is let go */
    }
    if (open_menu)
    {
        menu_open();
        app_draw_game(255);
        return;
    }
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        if (i == 0 && (fast || back))
            pads[i].buttons &= ~(BIT(BTN_L2) | BIT(BTN_R2));
        if (i == 0 && touch) /* the combo buttons go to SwanStationPS5, not the game */
            pads[i].buttons &= ~(chord ? combo_keys & ~BIT(BTN_R1) : combo_keys);
        pads[i].buttons = map_buttons(pads[i].buttons & ~BIT(BTN_MENU));
        if (chord_of[i])
            pads[i].buttons |= BIT(BTN_SELECT);
        if (touch_of[i] && !touch_used[i] && t_now - touch_since[i] >= select_hold_us)
            pads[i].buttons |= BIT(BTN_SELECT);
        if (select_frames[i] > 0)
        {
            pads[i].buttons |= BIT(BTN_SELECT);
            --select_frames[i];
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
    /* Optimal frame pacing: one frame per refresh, the sound stretched (at most 0.5 %) to keep the queue where it is.
     * Only when the screen's rate and the core's are within 3 % (else 0 and 2 frames per refresh, as before). */
    double sound_ratio = 1.0;
    bool optimal_active = false;
    {
        const double display_hz = plat_display_hz(), core_hz = host_fps();
        if (app.settings.frame_pacing && !app.settings.framegen && !back && !fast && display_hz > 20.0 && core_hz > 20.0 &&
            display_hz > core_hz * 0.97 && display_hz < core_hz * 1.03)
        {
            runs = 1;
            optimal_active = true;
            const double target = (double)per_frame * 3.0;
            double error = ((double)queued - target) / target * 0.05; /* too full: play a little faster */
            sound_ratio = 1.0 + (error < -0.005 ? -0.005 : error > 0.005 ? 0.005 : error);
        }
    }
    plat_audio_set_ratio(sound_ratio);
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
    static uint64_t emu_us, loops_since;
    static int emu_frames, loops;
    ++loops;
    /* what the pacing did, for comparing Optimal frame pacing on and off: refreshes by frames run, the sound's stretch */
    static int refreshes_by_runs[3], optimal_refreshes, pace_refreshes;
    static double ratio_min = 2.0, ratio_max = 0.0;
    if (!back && !fast)
    {
        ++refreshes_by_runs[runs < 0 ? 0 : runs > 2 ? 2 : runs];
        ++pace_refreshes;
        optimal_refreshes += optimal_active;
        if (optimal_active)
        {
            ratio_min = sound_ratio < ratio_min ? sound_ratio : ratio_min;
            ratio_max = sound_ratio > ratio_max ? sound_ratio : ratio_max;
        }
    }
    uint64_t emu_start = plat_ticks_us();
    /* Run-ahead: run the real frame, keep it, run 1 or 2 more to show where the
     * game is about to be (silently), then go back to the real one. What's on
     * screen answers the buttons that many frames sooner. */
    static void *ahead_state;
    static size_t ahead_size;
    /* run-ahead shows frames of a look-ahead that are not the game's own: Double frames needs the real ones */
    int ahead = !fast && !back && !app.settings.framegen ? app.settings.run_ahead : 0;
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
        {
            double core_hz = host_fps();
            SwanStationPS5_log("speed: game at %.1f%% (%.1f of %.2f fps), screen %.2f Hz, %.1f presents a second, Generate frames %s, frames shown for each of the game's: %d",
                               core_hz > 1.0 ? app.fps * 100.0 / core_hz : 0.0, app.fps, core_hz, plat_display_hz(),
                               loops_since && now > loops_since ? loops * 1000000.0 / (double)(now - loops_since) : 0.0,
                               app.settings.framegen == 2 ? "x4" : app.settings.framegen ? "x2" : "off",
                               plat_framegen_times() > 1 ? plat_framegen_times() : 1);
        }
        emu_us = 0;
        emu_frames = 0;
        loops = 0;
        loops_since = now;
        if (pace_refreshes)
        {
            SwanStationPS5_log("pacing: %s, %d refreshes: %d with no frame, %d with one, %d with two or more; sound speed %.4f to %.4f",
                               optimal_refreshes * 2 > pace_refreshes ? "optimal" : "usual", pace_refreshes,
                               refreshes_by_runs[0], refreshes_by_runs[1], refreshes_by_runs[2],
                               optimal_refreshes ? ratio_min : 1.0, optimal_refreshes ? ratio_max : 1.0);
            refreshes_by_runs[0] = refreshes_by_runs[1] = refreshes_by_runs[2] = 0;
            optimal_refreshes = pace_refreshes = 0;
            ratio_min = 2.0;
            ratio_max = 0.0;
        }
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
        double fg_hz = plat_framegen_hz();
        if (fg_hz > 1.0)
        {
            /* frames being generated: the frames the eye gets, each of the game's own and those generated, up to the screen's rate (25 -> 50) */
            double own = app.real_fps > 1.0f ? app.real_fps : app.fps;
            int times = plat_framegen_times();
            double shown = own * (times > 1 ? times : 2) < fg_hz ? own * (times > 1 ? times : 2) : fg_hz;
            snprintf(f, sizeof(f), "%.1f FPS (%s %.1f) | FG x%d", shown, tr("game"), own, times > 1 ? times : 2);
        }
        else if (app.real_fps > 0.0f && app.fps - app.real_fps >= 1.0f)
            snprintf(f, sizeof(f), "%.1f FPS (%s %.1f)", app.fps, tr("game"), app.real_fps);
        else
            snprintf(f, sizeof(f), "%.1f FPS", app.fps);
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
    GameStats *st = game_is_bios(app.game) ? NULL : stats_get(app.game->id);
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

#if defined(__PROSPERO__)
static int stop_services(void *done)
{
    remote_update(false);
    ra_shutdown();
    covers_stop();
    SDL_AtomicSet((SDL_atomic_t *)done, 1);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc)
            str_copy(launch_rom, sizeof(launch_rom), argv[++i]);
        else if (strcmp(argv[i], "--exit-after-game") == 0)
            exit_after_game = true;
    }
    /* First thing: proves the loader accepted the title and main() runs. (Turned off: the notice showed on screen.)
    plat_notify("SwanStationPS5 " SwanStationPS5_VERSION " starting...");
    */
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
    char video_out[8] = "";
    config_peek_string(app.paths.config, "video_out", video_out, sizeof(video_out));
    const bool high_refresh = atoi(video_out) == 1;
    if (high_refresh)
    {
        static const char *const params[] = {"/mnt/sandbox/PPSA98510_000/app0/sce_sys/param.json", "/app0/sce_sys/param.json",
                                             "/data/homebrew/PPSA98510/sce_sys/param.json"};
        for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); ++i)
        {
            FILE *f = fopen(params[i], "rb");
            if (!f)
                continue;
            fclose(f);
            setenv("PS5_VIDEOOUT_PARAM_JSON", params[i], 0);
            SwanStationPS5_log("screen: param.json for the 120 Hz mode: %s", params[i]);
            break;
        }
    }
    else
        setenv("PS5_VIDEOOUT_PARAM_JSON", "/nonexistent", 1);
    plat_want_high_refresh(high_refresh);
    plat_want_output(2); /* always 4K (1080p when the TV lacks the mode) */

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
    bios_choices_scan(app.paths.bios); /* before the settings are read: the BIOS options list the folder's files */
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
    /* a second games folder on each extended storage, only when it is mounted */
    char found_drives[256];
    app.drive_count = drives_list(found_drives, sizeof(found_drives), -1);
    for (int i = 0; i < app.drive_count; ++i)
        drives_list(app.drive_text[i], sizeof(app.drive_text[i]), i);
    SwanStationPS5_log("drives found: %s", app.drive_count ? found_drives : "none");
    const char *const *ext_drives = drive_paths;
    for (int i = 0; i < DRIVE_EXT_COUNT; ++i)
    {
        struct stat ext;
        char games[64];
        snprintf(games, sizeof(games), "%s/SwanStationPS5/games", ext_drives[i]);
        if (!drive_usable(i))
            SwanStationPS5_log("games folder %s: %s is not available", games, ext_drives[i]);
        else if (stat(games, &ext) == 0)
            SwanStationPS5_log("games folder %s: already exists", games);
        else if (make_dirs(games))
            SwanStationPS5_log("games folder %s: created successfully", games);
        else
            SwanStationPS5_log("games folder %s: could not be created (%s)", games, strerror(errno));
    }
    if (!stamp_is_fresh("disc-busy", 10)) /* the daemon may be writing a game right now */
    {
        discimport_cleanup(app.paths.games);
        for (int i = 0; i < DRIVE_EXT_COUNT; ++i)
        {
            if (!drive_usable(i))
                continue;
            char ext_games[SwanStationPS5_PATH_MAX];
            snprintf(ext_games, sizeof(ext_games), "%s/SwanStationPS5/games", drive_paths[i]);
            discimport_cleanup(ext_games);
        }
    }
    app_started_us = plat_ticks_us();
    if (stamp_is_fresh("daemon-launch", 90))
    {
        opened_by_daemon = true;
        char marker[SwanStationPS5_PATH_MAX];
        path_join(marker, sizeof(marker), app.paths.root, "daemon-launch");
        remove(marker);
        SwanStationPS5_log("started by the disc daemon: no notice sounds for the first seconds");
    }
    if (stamp_is_fresh("open-library", 60))
    {
        char marker[SwanStationPS5_PATH_MAX];
        path_join(marker, sizeof(marker), app.paths.root, "open-library");
        remove(marker);
        disc_after_game = true;
        SwanStationPS5_log("started again after a game: the disc in the drive is shown on the shelf, with no question");
    }
    SwanStationPS5_log("SwanStationPS5 %s starting, data root %s", SwanStationPS5_VERSION, app.paths.root);
    SwanStationPS5_log("screen: %s", plat_screen_info());
#if defined(__PROSPERO__)
    extern size_t ps5_heap_size_mb(void);
    SwanStationPS5_log("heap: %zu MB of address range%s", ps5_heap_size_mb(),
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
        remove_game_settings();
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
    {
        char sbi_dir[SwanStationPS5_PATH_MAX];
        path_join(sbi_dir, sizeof(sbi_dir), app.paths.cache, "nfs-sbi");
        nfs_set_sbi_dir(sbi_dir);
    }
    if (app.global.nfs_shares[0])
    {
        char nfs_error[256];
        if (!nfs_set_shares(app.global.nfs_shares, nfs_error, sizeof(nfs_error)))
            SwanStationPS5_log("nfs: %s", nfs_error);
    }
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
    if (launch_rom[0] && !app.storage_error[0])
    {
        const int index = game_for_launch_path(launch_rom);
        SwanStationPS5_log("forwarder: --rom %s -> %s%s", launch_rom,
                           index >= 0 ? app.library.games[index].path : "not in the library",
                           exit_after_game ? " (exit after the game)" : "");
        if (index >= 0)
        {
            shelf_select_game(index);
            app_start_game(index, false);
            started_from_argument = app.screen == SCREEN_GAME;
        }
    }
    SwanStationPS5_log("build: %s %s", __DATE__, __TIME__);
    /* (Turned off: the notice showed on screen.)
    plat_notify("SwanStationPS5 ready");
    */

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
        disc_auto_frame();
        if (!question_input(&pressed) && !sbi_check_input(&pressed) && !disc_ask_input(&pressed) && !play_input(&pressed) && !import_input(&pressed))
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
        /* the lights the app sets (player colours, cover colour, analog mode) are only on inside a game */
        if (!app.game && app.settings.lightbar != 0)
            for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
                plat_set_lightbar(i, 0x000000);
        switch (app.screen)
        {
        case SCREEN_LIBRARY: shelf_screen(pressed); break;
        case SCREEN_GAME:
            if (last_screen != SCREEN_GAME)
            {
                int connected = 0;
                uint32_t held = 0;
                for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
                {
                    held_from_menu |= pads[i].buttons;
                    held |= pads[i].buttons;
                    connected += pads[i].connected;
                }
                if (last_screen != SCREEN_MENU && last_screen != SCREEN_SETTINGS)
                {
                    input_reported = false;
                    input_start_us = plat_ticks_us();
                    SwanStationPS5_log("input: game screen, %d pad(s) connected, buttons held at the start 0x%x", connected, held);
                }
            }
            game_screen(pads);
            break;
        case SCREEN_MENU: menu_screen(pressed); break;
        case SCREEN_SETTINGS:
            if (app.game && !app.settings_preview && (pressed & BIT(BTN_MENU)) && settings_preview_allowed())
            {
                app.settings_preview = true;
                app.settings_preview_time = 0;
                if (app.settings.manual_crop)
                    manual_crop_fix();
            }
            else if (app.settings_preview && (pressed & BIT(BTN_MENU)))
            {
                app.settings_preview = false;
                if (app.settings.manual_crop)
                    app_save_settings();
                sfx_play(SFX_BACK);
            }
            if (app.settings_preview && app.game)
            {
                app.settings_preview_time += app.dt;
                /* the game keeps running, without the pad: its picture, with the settings as they are now */
                PadState idle[SwanStationPS5_MAX_PADS];
                memcpy(idle, pads, sizeof(idle));
                for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
                {
                    idle[i].buttons = 0;
                    idle[i].lx = idle[i].ly = idle[i].rx = idle[i].ry = 0;
                    idle[i].l2 = idle[i].r2 = 0;
                    idle[i].touching = false;
                    idle[i].mouse_dx = idle[i].mouse_dy = 0;
                }
                if (app.settings.manual_crop)
                {
                    const int up = (pressed & BIT(BTN_UP)) != 0, down = (pressed & BIT(BTN_DOWN)) != 0,
                              left = (pressed & BIT(BTN_LEFT)) != 0, right = (pressed & BIT(BTN_RIGHT)) != 0;
                    if (pressed & BIT(BTN_SQUARE))
                        manual_crop_edit(0, 0, true);
                    else
                        manual_crop_edit(right - left, 2 * (down - up), false);
                }
                game_screen(idle);
                if (app.settings.manual_crop)
                {
                    const float t = app.settings_preview_time, in = t < 0.25f ? t / 0.25f : 1.0f;
                    const float w = 900, h = 230, x = (plat_width() - w) * 0.5f, y = plat_height() - h - 36 + (1.0f - in) * 60.0f;
                    char values[96], ratio[24];
                    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), in));
                    text_draw_fit(x + 36, y + 22, 26, FONT_BOLD, argb_alpha(TH_TEXT, in), ALIGN_LEFT, w - 72,
                                  tr("Manual crop"));
                    text_draw_fit(x + 36, y + 66, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT, w - 72,
                                  tr("{Left} {Right}  Stretch or narrow the picture, keeping its aspect ratio"));
                    text_draw_fit(x + 36, y + 102, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT, w - 72,
                                  tr("{Up} {Down}  Move the picture up or down"));
                    text_draw_fit(x + 36, y + 138, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT, w - 72,
                                  tr("{Square}  Back to the original area"));
                    crop_ratio_text(ratio, sizeof(ratio),
                                    (play_widescreen_active() ? 16.0f / 9.0f : host_aspect()) * app.settings.mc_w /
                                        app.settings.mc_h);
                    snprintf(values, sizeof(values), "%d%%   %+d   %s", app.settings.mc_w, app.settings.mc_y, ratio);
                    text_draw_fit(x + 36, y + 180, 22, FONT_BOLD, argb_alpha(TH_TEXT, in), ALIGN_LEFT, w - 72, values);
                }
                {
                    const float t = app.settings_preview_time, in = t < 0.25f ? t / 0.25f : 1.0f,
                                slide = (1.0f - in) * (1.0f - in) * 80.0f;
                    const float w = 660, h = 116, x = (plat_width() - w) * 0.5f, y = -slide;
                    draw_rrect(x, y - TH_RADIUS, w, h + TH_RADIUS, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), in));
                    icon_draw(ICON_DEVICE_TV, x + 30, y + 30, 56, argb_alpha(TH_TEXT, in));
                    text_draw_fit(x + 120, y + 20, 30, FONT_BOLD, argb_alpha(TH_TEXT, in), ALIGN_LEFT, w - 140,
                                  tr("Preview"));
                    text_draw(x + 120, y + 66, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT,
                              tr("Press {Touchpad} to return to settings..."));
                }
                app_draw_toast();
            }
            else
                settings_screen(pressed);
            break;
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
        {
            const bool nostalgia = app.game && app.global.nostalgia && app.screen == SCREEN_GAME;
            sfx_silence(nostalgia);
        }
        {
            static const int nfs_seconds[] = {5, 10, 15, 20, 30};
            static bool nfs_told;
            nfs_set_read_timeout(nfs_seconds[app.global.nfs_timeout >= 0 && app.global.nfs_timeout < 5 ? app.global.nfs_timeout : 2]);
            const bool again = nfs_reconnecting() && app.game;
            if (again && !nfs_told)
                app_toast("Reconnecting to the NFS server.");
            nfs_told = again;
        }
        if (nfs_connection_lost() && app.game)
        {
            app_stop_game();
            app_toast("NFS communication was lost.");
            app_rescan();
        }
        if (ntfsfs_connection_lost() && app.game)
        {
            app_stop_game();
            app_toast("The NTFS disk was disconnected.");
            app_rescan();
        }
        rescan_async_poll();
        if (nfs_mount_finished() && !app.game)
        {
            nfs_rescan_from_mount = true;
            app_rescan();
            nfs_rescan_from_mount = false;
            if (nfs_notify)
            {
                nfs_notify = false;
                char detail[400], message[500];
                const int why = nfs_first_failure(detail, sizeof(detail));
                switch (why)
                {
                case NFS_FAIL_NONE: app_toast("NFS share connected."); break;
                case NFS_FAIL_NO_ANSWER: app_toast_for(tr("NFS share not connected: the server did not answer."), 6.0f); break;
                case NFS_FAIL_REFUSED: app_toast_for(tr("NFS share not connected: the server refused the mount."), 6.0f); break;
                case NFS_FAIL_NO_FOLDER: app_toast_for(tr("NFS share not connected: the folder is not on the server."), 6.0f); break;
                case NFS_FAIL_CONNECT:
                    app_toast_for(tr("NFS share not connected: the connection was refused or could not be made."), 6.0f);
                    break;
                case NFS_FAIL_CLOSED:
                    app_toast_for(tr("NFS share not connected: the server closed the connection."), 6.0f);
                    break;
                case NFS_FAIL_NAME:
                    app_toast_for(tr("NFS share not connected: the server's name could not be found."), 6.0f);
                    break;
                case NFS_FAIL_DENIED:
                    app_toast_for(tr("NFS share not connected: the server denies access to this console."), 6.0f);
                    break;
                case NFS_FAIL_NOT_EXPORTED:
                    snprintf(message, sizeof(message), "%s %.300s",
                             tr("NFS share not connected: that folder is not exported by the server. It exports:"), detail);
                    app_toast_for(message, 8.0f);
                    break;
                default:
                    snprintf(message, sizeof(message), "%s %.300s", tr("NFS share not connected:"), detail);
                    app_toast_for(message, 6.0f);
                    break;
                }
            }
        }
        if (cheats_fetch_finished() && app.game && !ra_hardcore() &&
            cheats_load(&app.cheats, app.game, app.paths.cheats))
        {
            cheats_apply(&app.cheats);
            play_widescreen(); /* the file may hold the game's widescreen code */
        }
        if (app.screen != SCREEN_SETTINGS)
            app.settings_preview = false;
        if (app.screen != SCREEN_GAME)
            ra_idle();
        draw_achievement();
        sbi_check_draw();
        play_draw();
        import_draw();
        disc_ask_draw();
        message_box_draw();
        question_draw();
        toast_render();
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
#if !defined(__PROSPERO__)
    /* the console takes the title down with its threads and sockets: waiting for them (a cover download, a
     * RetroAchievements request, the phone's server) could leave the screen frozen for up to half a minute */
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
#endif
#if defined(__PROSPERO__)
    /* The system closes the title: exit()/_exit() raise SIGSYS on the console. As ProsperoStore does it: LoadExec
     * "exit" ends the title normally. */
    extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
    /* extern int sceSystemServiceGetAppIdOfRunningBigApp(void);
     * extern int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump); */
    SwanStationPS5_log("shutdown: closing the title");
    {
        static SDL_atomic_t services_done;
        SDL_Thread *stopper = SDL_CreateThread(stop_services, "stop-services", &services_done);
        for (int waited = 0; stopper && !SDL_AtomicGet(&services_done) && waited < 2000; waited += 10)
            plat_sleep_us(10000);
        if (stopper && SDL_AtomicGet(&services_done))
        {
            SDL_WaitThread(stopper, NULL);
            SwanStationPS5_log("shutdown: network and RetroAchievements stopped");
        }
        else
        {
            if (stopper)
                SDL_DetachThread(stopper);
            SwanStationPS5_log("shutdown: services did not stop in 2 s, closing anyway");
        }
    }
    icons_shutdown();
    text_shutdown();
    library_free(&app.library);
    SwanStationPS5_log("shutdown: interface freed");
    plat_release();
    fflush(NULL);
    for (;;)
    {
        /* sceSystemServiceKillApp(sceSystemServiceGetAppIdOfRunningBigApp(), -1, 0, 0); */
        int rc = sceSystemServiceLoadExec("exit", NULL);
        SwanStationPS5_log("shutdown: LoadExec exit refused rc=0x%x", (unsigned)rc);
        plat_sleep_us(1000000); /* until the shell takes the title down; asked again if it did not */
    }
#endif
    return 0;
}
