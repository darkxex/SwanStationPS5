/*
 * PSXS5 - the game shelf (home screen).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers stand on a shelf: the selected one faces you, its neighbours turn
 * away towards the edges. Categories (L1/R1) filter it, OPTIONS sorts it, and
 * the background takes the colour of the selected cover. Everything here is
 * drawn with blits and fills, the software renderer's fast paths.
 */
#include "coverflow.h"

#include "../app.h"
#include "../art.h"
#include "../config.h"
#include "../covers.h"
#include "../gamedb.h"
#include "../play.h"
#include "../profiles.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "../ra/achievements.h"
#include "../stats.h"
#include "../tips.h"
#include "../update.h"
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
#include <time.h>

#define CENTER_X 960.0f
#define CENTER_Y 430.0f
#define COVER_H 500.0f
#define SIDE_GAP 400.0f  /* centre to the first neighbour */
#define STACK_GAP 150.0f /* between further neighbours */
#define SIDE_SCALE 0.74f
#define SIDE_SQUEEZE 0.58f /* turned-away covers look narrower */
#define LAUNCH_TIME 0.3f /* the fade to black */
#define DISC_TIME 0.95f  /* before it, with the disc animation: the disc slides out and spins up */

static float launch_total(void)
{
    return app.global.disc_animation ? DISC_TIME + LAUNCH_TIME : LAUNCH_TIME;
}

static struct
{
    int *view; /* library indices on the shelf, in order */
    int view_count, view_capacity;
    int cursor; /* position in view */
    float pos;  /* animated position */
    bool details;
    float details_t, launch_t;
    bool starting; /* the launch is over: this frame shows "Loading", the next one loads */
    float chip_x, chip_w; /* animated category highlight */
    float tint[3];        /* animated background colour */
    float title_fade;     /* text fades in after a move */
    int last_cursor_game;
    /* "Continue or start over" when a game has a quick-resume save */
    bool dialog, resume;
    int dialog_choice;
    float dialog_t;
    long resume_age;
    PlatTexture *resume_thumb;
} S;

static const char *const CATEGORY_NAMES[CAT_COUNT] = {
    "All games", "Recently played", "Favorites", "Multi-disc", "USA", "Europe", "Japan", "2+ players", "Hidden"};
static const char *const SORT_NAMES[SORT_COUNT] = {"Title", "Recently played", "Most played",
                                                   "Region"};

const char *shelf_sort_name(int sort)
{
    return tr(SORT_NAMES[sort >= 0 && sort < SORT_COUNT ? sort : 0]);
}

/* 0 unknown, 1 USA, 2 Europe, 3 Japan */
static int region_of(const char *serial)
{
    if (!serial[0])
        return 0;
    if (!strncmp(serial, "SLUS", 4) || !strncmp(serial, "SCUS", 4) || !strncmp(serial, "PAPX", 4))
        return 1;
    if (!strncmp(serial, "SLES", 4) || !strncmp(serial, "SCES", 4) || !strncmp(serial, "SCED", 4))
        return 2;
    return 3;
}

const char *shelf_region_name(const char *serial)
{
    static const char *const names[] = {"Unknown region", "USA", "Europe", "Japan"};
    return tr(names[region_of(serial)]);
}

/* ---------------------------------------------------------------- the view */

static bool in_category(const Game *g, int category)
{
    GameStats *st = stats_get(g->id);
    if (st && st->hidden) /* hidden games are only under Hidden */
        return category == CAT_HIDDEN;
    switch (category)
    {
    case CAT_HIDDEN: return false;
    case CAT_RECENT: return st && st->last_played > 0;
    case CAT_FAVORITES: return st && st->favorite;
    case CAT_MULTI_DISC: return g->discs > 1;
    case CAT_USA: return region_of(g->serial) == 1;
    case CAT_EUROPE: return region_of(g->serial) == 2;
    case CAT_JAPAN: return region_of(g->serial) == 3;
    case CAT_MULTIPLAYER:
    {
        const GameInfo *info = gamedb_get(g->serial);
        return info && info->max_players > 1;
    }
    default: return true;
    }
}

static int category_size(int category)
{
    int n = 0;
    for (int i = 0; i < app.library.count; ++i)
        n += in_category(&app.library.games[i], category);
    return n;
}

static int sort_mode;

static int compare(const void *pa, const void *pb)
{
    const Game *a = &app.library.games[*(const int *)pa], *b = &app.library.games[*(const int *)pb];
    GameStats *sa = stats_get(a->id), *sb = stats_get(b->id);
    switch (sort_mode)
    {
    case SORT_RECENT:
    {
        int64_t la = sa ? sa->last_played : 0, lb = sb ? sb->last_played : 0;
        if (la != lb)
            return la < lb ? 1 : -1;
        break;
    }
    case SORT_MOST_PLAYED:
    {
        uint32_t ta = sa ? sa->seconds : 0, tb = sb ? sb->seconds : 0;
        if (ta != tb)
            return ta < tb ? 1 : -1;
        break;
    }
    case SORT_REGION:
    {
        int ra = region_of(a->serial), rb = region_of(b->serial);
        if (ra != rb)
            return ra - rb;
        break;
    }
    default:
        break;
    }
    int t = str_icmp(a->title, b->title);
    return t ? t : (*(const int *)pa - *(const int *)pb);
}

static void build_view(int keep_game)
{
    Settings *g = &app.global;
    if (g->shelf_category != CAT_ALL && category_size(g->shelf_category) == 0)
        g->shelf_category = CAT_ALL;
    if (S.view_capacity < app.library.count)
    {
        int *grown = realloc(S.view, sizeof(int) * (size_t)(app.library.count + 16));
        if (!grown)
            return;
        S.view = grown;
        S.view_capacity = app.library.count + 16;
    }
    S.view_count = 0;
    for (int i = 0; i < app.library.count; ++i)
        if (in_category(&app.library.games[i], g->shelf_category))
            S.view[S.view_count++] = i;
    sort_mode = g->shelf_category == CAT_RECENT ? SORT_RECENT : g->sort_mode;
    qsort(S.view, (size_t)S.view_count, sizeof(int), compare);
    S.cursor = 0;
    for (int k = 0; k < S.view_count; ++k)
        if (S.view[k] == keep_game)
            S.cursor = k;
    S.pos = (float)S.cursor;
}

static int selected_game(void)
{
    return S.view_count ? S.view[S.cursor] : -1;
}

void shelf_init(int last_game)
{
    S.last_cursor_game = last_game;
    srand((unsigned)time(NULL)); /* Surprise me */
    S.tint[0] = 0x3a / 255.0f;
    S.tint[1] = 0x50 / 255.0f;
    S.tint[2] = 0xc8 / 255.0f;
}

void shelf_library_changed(void)
{
    build_view(S.last_cursor_game);
}

void shelf_select_game(int library_index)
{
    S.last_cursor_game = library_index;
    build_view(library_index);
}

/* ---------------------------------------------------------------- backdrop */

/* A neutral vertical gradient, built once and blitted tinted each frame:
 * a full-screen gradient mesh per frame would be costly here. */
/* The theme's background as a grey picture; plat_draw_backdrop colours it
 * (the cover's colour for Classic, the theme's otherwise). */
static void make_backdrop(uint8_t *grey, int W, int H, int style, bool light)
{
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float t = (float)y / (H - 1), dx = (x - W * 0.5f) / (W * 0.5f), v;
            switch (style)
            {
            case BACKDROP_GRID:
            {
                /* a dark room, a glowing horizon and a grid floor running away */
                const float horizon = 0.50f;
                v = 0.05f + 0.05f * (1.0f - t);
                float d = (t - horizon) * H;
                v += 0.85f * expf(-d * d / 18.0f) + 0.18f * expf(-d * d / 900.0f);
                if (t > horizon + 0.004f)
                {
                    float depth = 60.0f / (t - horizon);          /* far rows bunch up */
                    float wx = dx * depth * 0.5f;                  /* columns converge */
                    float fz = depth / 12.0f - floorf(depth / 12.0f), fx = wx / 6.0f - floorf(wx / 6.0f);
                    float line = fminf(fminf(fz, 1.0f - fz) * 12.0f, fminf(fx, 1.0f - fx) * 40.0f / (1.0f + depth * 0.02f));
                    float glow = line < 1.0f ? (1.0f - line) : 0.0f;
                    v += glow * 0.75f * fminf(1.0f, (t - horizon) * 6.0f);
                }
                break;
            }
            case BACKDROP_FLAT:
                v = light ? 0.99f - t * 0.05f : 0.42f - t * 0.08f;
                v *= 1.0f - dx * dx * (light ? 0.02f : 0.12f);
                break;
            case BACKDROP_LAMP:
            {
                /* a warm pool of light from a lamp above the shelf */
                float lx = (x - W * 0.5f) / 760.0f, ly = (y - H * 0.24f) / 470.0f;
                v = 0.12f + 0.62f * expf(-(lx * lx + ly * ly));
                break;
            }
            default:
            {
                /* dark top, brighter band behind the covers, dark floor, soft vignette */
                float l = t < 0.42f ? 0.30f + t / 0.42f * 0.42f
                                    : t < 0.64f ? 0.72f - (t - 0.42f) / 0.22f * 0.30f
                                                : 0.42f - (t - 0.64f) / 0.36f * 0.30f;
                v = l * (1.0f - dx * dx * 0.35f);
            }
            }
            grey[(size_t)y * W + x] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f);
        }
}

void shelf_backdrop(void)
{
    enum { W = 1920, H = 1080 };
    static uint8_t *grey;
    static int made = -1; /* style * 2 + light */
    int key = theme.backdrop * 2 + (theme.light ? 1 : 0);
    if (!grey)
        grey = malloc((size_t)W * H);
    if (!grey)
    {
        draw_rect(0, 0, plat_width(), plat_height(), TH_BG);
        return;
    }
    if (made != key)
    {
        make_backdrop(grey, W, H, theme.backdrop, theme.light);
        made = key;
    }
    uint32_t tint = theme.backdrop_tint
                        ? theme.backdrop_tint
                        : 0xff000000u | (uint32_t)(S.tint[0] * 255) << 16 | (uint32_t)(S.tint[1] * 255) << 8 |
                              (uint32_t)(S.tint[2] * 255);
    plat_draw_backdrop(grey, W, H, tint);
}

static void update_tint(int game)
{
    /* Dark: deep navy with a hint of the cover. Cover colour: the theme's blue
     * pulled most of the way towards the cover's own colour. */
    bool dark = app.global.background == 0;
    float target[3] = {0x3a / 255.0f, 0x50 / 255.0f, 0xc8 / 255.0f};
    if (dark)
    {
        target[0] = 0x1c / 255.0f;
        target[1] = 0x24 / 255.0f;
        target[2] = 0x5e / 255.0f;
    }
    uint32_t c = game >= 0 ? covers_color(game) : 0;
    if (c)
    {
        float cover[3] = {((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f};
        float m = fmaxf(cover[0], fmaxf(cover[1], cover[2]));
        float mix = dark ? 0.22f : 0.55f, level = dark ? 0.32f : 0.8f;
        for (int i = 0; i < 3; ++i)
        {
            float v = m > 0.05f ? cover[i] / m * level : level * 0.6f; /* keep it bright enough */
            target[i] = target[i] * (1.0f - mix) + v * mix;
        }
    }
    for (int i = 0; i < 3; ++i)
        anim_approach(&S.tint[i], target[i], app.dt, 4.0f);
}

/* ---------------------------------------------------------------- pieces */

/* RetroAchievements progress in a cover's bottom-right corner: gold once
 * mastered. Nothing for games without achievements or not played yet. */
static void draw_ach_badge(const Game *g, float right, float bottom, float size)
{
    GameStats *st = stats_get(g->id);
    if (!st || st->ach_total <= 0 || st->ach_unlocked < 0)
        return;
    bool done = st->ach_unlocked >= st->ach_total;
    char t[24];
    snprintf(t, sizeof(t), "%d/%d", st->ach_unlocked, st->ach_total);
    float h = size * 1.7f, tw = text_width(size, FONT_BOLD, t) + h + size * 0.9f;
    float x = right - tw - size * 0.4f, y = bottom - h - size * 0.4f;
    draw_rrect(x, y, tw, h, h * 0.5f, done ? 0xf0c8961eu : 0xd0101018u);
    icon_draw(ICON_TROPHY, x + h * 0.22f, y + h * 0.18f, h * 0.64f, done ? 0xffffffffu : 0xfff0b429u);
    text_draw(x + tw - size * 0.6f, y + (h - size) * 0.5f - 1, size, FONT_BOLD, 0xffffffffu, ALIGN_RIGHT, t);
}

static void draw_cover(PlatTexture *tex, const Game *g, float cx, float cy, float h, float squeeze,
                       uint32_t tint, bool selected)
{
    int tw = 1, th = 1;
    plat_texture_size(tex, &tw, &th);
    float aspect = tex ? (float)tw / th : 0.88f;
    float w = h * aspect * squeeze, x = cx - w * 0.5f, y = cy - h * 0.5f;
    if (selected)
    {
        draw_rrect(x - 10, y + 14, w + 20, h + 6, 18, 0x60000000u); /* shadow */
        if (theme.backdrop == BACKDROP_GRID) /* a neon glow */
            draw_rrect_outline(x - 14, y - 14, w + 28, h + 28, 20, 6, (theme.cover_outline & 0xffffffu) | 0x50000000u);
        /* Classic: no white border around the selected cover (the other themes keep theirs) */
        if (theme_current() != THEME_CLASSIC)
            draw_rrect_outline(x - 7, y - 7, w + 14, h + 14, 14, 4, theme.cover_outline);
    }
    if (tex) /* flat covers are opaque: a plain copy is much cheaper than blending */
        plat_draw_texture(tex, x, y, w, h, tint, app.global.cover_style == COVER_BOX3D);
    else
    {
        draw_rrect(x, y, w, h, 10, TH_CARD);
        text_draw_fit(cx, cy - 16, 24, FONT_BOLD, TH_TEXT_DIM, ALIGN_CENTER, w - 24, g->title);
    }
    if (selected)
        draw_ach_badge(g, x + w, y + h, 20);
}

static void draw_details(const Game *g, float t)
{
    if (t <= 0.0f)
        return;
    float ease = 1.0f - (1.0f - t) * (1.0f - t);
    float w = 620, x = plat_width() - (w + 48) * ease, y = 130, h = 830;
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(theme.light ? 0xff : 0xf2), t));
    text_draw_fit(x + 40, y + 34, 32, FONT_BOLD, argb_alpha(TH_TEXT, t), ALIGN_LEFT, w - 80, g->title);
    GameStats *st = stats_get(g->id);
    char played[48] = "", when[48] = "", ach[48] = "", discs[16];
    if (st)
    {
        stats_format_time(st->seconds, played, sizeof(played));
        stats_format_when(st->last_played, when, sizeof(when));
        if (st->ach_unlocked >= 0 && st->ach_total > 0)
            snprintf(ach, sizeof(ach), "%d / %d", st->ach_unlocked, st->ach_total);
    }
    snprintf(discs, sizeof(discs), "%d", g->discs);
    const char *folder = strrchr(g->folder, '/');
    /* what DuckStation's database knows: genre, release, players */
    const GameInfo *info = gamedb_get(g->serial);
    char released[160] = "", players[48] = "";
    if (info && info->year)
        snprintf(released, sizeof(released), info->developer[0] ? "%d  \xc2\xb7  %s" : "%d", info->year, info->developer);
    if (info && info->max_players > 1)
        snprintf(players, sizeof(players), tr(info->flags & GDB_MULTITAP ? "1 to %d (multitap)" : "1 to %d"),
                 info->max_players);
    else if (info && info->max_players == 1)
        str_copy(players, sizeof(players), "1");
    struct { const char *label; const char *value; int icon; } rows[] = {
        {"Serial", g->serial[0] ? g->serial : tr("Unknown"), ICON_CARDS},
        {"Region", shelf_region_name(g->serial), ICON_WORLD},
        {"Genre", info && info->genre[0] ? info->genre : tr("Unknown"), ICON_CATEGORY},
        {"Released", released[0] ? released : tr("Unknown"), ICON_CLOCK},
        {"Players", players[0] ? players : tr("Unknown"), ICON_USER},
        {"Played", played[0] ? played : tr("Not yet"), ICON_HISTORY},
        {"Achievements", ach[0] ? ach : tr("Unknown"), ICON_TROPHY},
        {"Folder", folder ? folder + 1 : g->folder, ICON_FOLDER},
    };
    (void)when;
    for (int i = 0; i < 8; ++i)
    {
        float ry = y + 92 + i * 44;
        icon_draw(rows[i].icon, x + 40, ry + 1, 26, argb_alpha(TH_FOCUS, t));
        text_draw(x + 84, ry, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, t), ALIGN_LEFT, tr(rows[i].label));
        text_draw_fit(x + 310, ry, 22, FONT_BOLD, argb_alpha(TH_TEXT, t), ALIGN_LEFT, w - 350,
                      rows[i].value);
    }
    /* the title screen and a moment of play (downloaded with the covers) */
    {
        const float pw = (w - 80 - 16) * 0.5f, ph = pw * 0.75f, py = y + 452;
        const int kinds[2] = {ART_TITLE, ART_SNAP};
        for (int k = 0; k < 2; ++k)
        {
            float px = x + 40 + k * (pw + 16);
            PlatTexture *pic = art_get(g, kinds[k]);
            draw_rrect(px, py, pw, ph, TH_RADIUS_SMALL, argb_alpha(TH_BG_DEEP, t));
            if (pic)
            {
                int tw, th;
                plat_texture_size(pic, &tw, &th);
                float s = fminf(pw / tw, ph / th), dw = tw * s, dh = th * s;
                plat_draw_texture(pic, px + (pw - dw) * 0.5f, py + (ph - dh) * 0.5f, dw, dh,
                                  argb_alpha(0xffffffffu, t), true);
            }
            else
                icon_draw(art_pending(g, kinds[k]) ? ICON_DOWNLOAD : ICON_PHOTO, px + pw * 0.5f - 18,
                          py + ph * 0.5f - 18, 36, argb_alpha(TH_TEXT_DIM, t * 0.6f));
        }
    }
    /* tips: worked out once per game (they read files), with its own settings */
    static char tips[3][TIP_LEN];
    static int tip_count;
    static const Game *tips_game;
    if (g != tips_game)
    {
        tips_game = g;
        Settings s;
        char own[PSXS5_PATH_MAX];
        app_game_config_path(own, sizeof(own), g);
        config_load_game(&s, &app.global, own);
        tip_count = tips_for(g, &s, tips, 3);
    }
    for (int i = 0; i < tip_count; ++i)
    {
        float ty = y + 668 + i * 34;
        icon_draw(ICON_INFO_CIRCLE, x + 40, ty + 2, 24, argb_alpha(TH_GOLD, t));
        text_draw_fit(x + 76, ty, 19, FONT_REGULAR, argb_alpha(TH_TEXT_SOFT, t), ALIGN_LEFT, w - 116, tips[i]);
    }
    if (t > 0.5f)
        draw_pad_glyph(GLYPH_SQUARE, x + 54, y + h - 46, 28);
    text_draw(x + 78, y + h - 58, 22, FONT_REGULAR, argb_alpha(TH_TEXT, t), ALIGN_LEFT,
              tr("Choose a cover"));
}

/* Category chips along the top; the highlight slides between them. */
static void draw_header(void)
{
    text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, PSXS5_TITLE);
    float x = TH_MARGIN + text_width(44, FONT_BOLD, PSXS5_TITLE) + 40, y = 46, h = 46;
    for (int c = 0; c < CAT_COUNT; ++c)
    {
        int n = category_size(c);
        if (c != CAT_ALL && n == 0)
            continue;
        char label[64];
        if (c == CAT_ALL)
            snprintf(label, sizeof(label), "%s  %d", tr(CATEGORY_NAMES[c]), n);
        else
            str_copy(label, sizeof(label), tr(CATEGORY_NAMES[c]));
        float w = text_width(22, FONT_REGULAR, label) + 40;
        bool on = c == app.global.shelf_category;
        if (on)
        {
            if (S.chip_w == 0)
                S.chip_x = x, S.chip_w = w;
            anim_approach(&S.chip_x, x, app.dt, TH_SNAP);
            anim_approach(&S.chip_w, w, app.dt, TH_SNAP);
            draw_rrect(S.chip_x, y, S.chip_w, h, h * 0.5f, TH_TEXT);
        }
        text_draw(x + 20, y + 11, 22, FONT_REGULAR, on ? TH_BG : TH_HINT, ALIGN_LEFT, label);
        x += w + 6;
    }

    /* account and clock, top right */
    char clock_text[16];
    plat_clock(clock_text, sizeof(clock_text)); /* the console's time zone and 12/24 h */
    float rx = plat_width() - TH_MARGIN;
    text_draw(rx, 53, 24, FONT_REGULAR, TH_TEXT_SOFT, ALIGN_RIGHT, clock_text);
    rx -= text_width(24, FONT_REGULAR, clock_text) + 32;
    if (update_state() == UPDATE_AVAILABLE || update_state() == UPDATE_INSTALLED)
    {
        char up[64];
        snprintf(up, sizeof(up), tr(update_state() == UPDATE_INSTALLED ? "Restart for %s" : "Update %s"),
                 update_version());
        float uw = text_width(20, FONT_BOLD, up) + 64;
        draw_rrect(rx - uw, 44, uw, 46, 23, 0xff2a2410u);
        icon_draw(ICON_DOWNLOAD, rx - uw + 14, 52, 30, TH_GOLD);
        text_draw(rx - uw + 50, 56, 20, FONT_BOLD, TH_GOLD, ALIGN_LEFT, up);
        rx -= uw + 24;
    }
    if (profiles_count() > 1)
    {
        const char *name = profiles_name(profiles_current());
        float pw = text_width(22, FONT_BOLD, name) + 66;
        draw_rrect(rx - pw, 44, pw, 46, 23, TH_PILL);
        icon_draw(ICON_USER, rx - pw + 14, 52, 28, TH_FOCUS);
        text_draw(rx - pw + 50, 56, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, name);
        rx -= pw + 24;
    }
    if (ra_user()[0])
    {
        char who[96];
        snprintf(who, sizeof(who), "%s  \xc2\xb7  %u", ra_user(), ra_user_score());
        text_draw(rx, 53, 24, FONT_REGULAR, TH_TEXT_SOFT, ALIGN_RIGHT, who);
        rx -= text_width(24, FONT_REGULAR, who) + 38;
        icon_draw(ICON_TROPHY, rx, 52, 30, TH_GOLD);
    }
}

static void draw_info(const Game *g, float alpha)
{
    GameStats *st = stats_get(g->id);
    float y = 730;
    float tw = text_width(52, FONT_BOLD, g->title);
    if (st && st->favorite)
        icon_draw(ICON_STAR, CENTER_X - fminf(tw, 1500) * 0.5f - 52, y + 10, 40,
                  argb_alpha(TH_GOLD, alpha));
    text_draw_fit(CENTER_X, y, 52, FONT_BOLD, argb_alpha(TH_TEXT, alpha), ALIGN_CENTER, 1500, g->title);

    /* tags: region, serial, discs, play time, last played */
    char discs[32], played[48] = "", played_tag[64] = "", when[48] = "";
    snprintf(discs, sizeof(discs), tr(g->discs == 1 ? "%d disc" : "%d discs"), g->discs);
    if (st)
    {
        stats_format_time(st->seconds, played, sizeof(played));
        if (played[0])
            snprintf(played_tag, sizeof(played_tag), tr("Played %s"), played);
        stats_format_when(st->last_played, when, sizeof(when));
    }
    const char *tags[5] = {shelf_region_name(g->serial), g->serial[0] ? g->serial : tr("No serial"),
                           discs, played_tag, when};
    float widths[5], total = 0;
    for (int i = 0; i < 5; ++i)
        if (tags[i][0])
        {
            widths[i] = text_width(22, FONT_REGULAR, tags[i]) + 36;
            total += widths[i] + 10;
        }
        else
            widths[i] = 0;
    float x = CENTER_X - (total - 10) * 0.5f, ty = y + 84;
    for (int i = 0; i < 5; ++i)
        if (widths[i] > 0)
            x += draw_pill(x, ty, 40, 22, argb_alpha(TH_PILL_A(0xc0), alpha),
                           argb_alpha(TH_TEXT_SOFT, alpha), tags[i]) + 10;

    /* achievement progress, when known */
    if (st && st->ach_unlocked >= 0 && st->ach_total > 0)
    {
        float bw = 340, bx = CENTER_X - bw * 0.5f + 20, by = ty + 70;
        icon_draw(ICON_TROPHY, bx - 46, by - 12, 30, argb_alpha(TH_GOLD, alpha));
        draw_rrect(bx, by, bw, 8, 4, argb_alpha(TH_PILL, alpha));
        float f = (float)st->ach_unlocked / st->ach_total;
        draw_rrect(bx, by, bw * (f > 1 ? 1 : f), 8, 4, argb_alpha(TH_GOLD, alpha));
        char a[32];
        snprintf(a, sizeof(a), "%d / %d", st->ach_unlocked, st->ach_total);
        text_draw(bx + bw + 16, by - 13, 22, FONT_REGULAR, argb_alpha(TH_TEXT_SOFT, alpha), ALIGN_LEFT, a);
    }
}

/* ---------------------------------------------------------------- screen */

static void storage_screen(uint32_t pressed)
{
    shelf_backdrop();
    text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, PSXS5_TITLE);
    float w = 1200, h = 300, x = CENTER_X - w * 0.5f, y = 330;
    draw_rrect(x, y, w, h, TH_RADIUS, TH_CARD_SOFT);
    icon_draw(ICON_ALERT_TRIANGLE, CENTER_X - 32, y + 40, 64, TH_GOLD);
    text_draw(CENTER_X, y + 124, 36, FONT_BOLD, TH_TEXT, ALIGN_CENTER, tr("SwanStationPS5 can't open /data/PSXS5"));
    text_draw_fit(CENTER_X, y + 190, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, w - 80,
                  app.storage_error);
    static const int glyphs[] = {GLYPH_SQUARE};
    static const char *const labels[] = {"Settings"};
    app_draw_hints(glyphs, labels, 1, NULL);
    if (pressed & BIT(BTN_SQUARE))
        app_open_settings(SCREEN_LIBRARY);
    app_draw_toast();
}

static void change_category(int step)
{
    int c = app.global.shelf_category;
    for (int i = 0; i < CAT_COUNT; ++i)
    {
        c = (c + step + CAT_COUNT) % CAT_COUNT;
        if (c == CAT_ALL || category_size(c) > 0)
            break;
    }
    if (c == app.global.shelf_category)
        return;
    int keep = selected_game();
    app.global.shelf_category = c;
    build_view(keep);
    S.title_fade = 0;
    sfx_play(SFX_CLICK);
}

/* ---------------------------------------------------------------- cover picker */

/* Choose a game's cover from the images in covers/ and in the game's folder
 * (GitHub issue #1). The pick is copied to covers/custom/<id>.<ext>, which
 * the cover lookup tries first; "Automatic" removes it. */
#define PICK_MAX 400
#define PICK_ROWS 11

static struct
{
    bool open;
    float t;
    int game;
    int count, cursor; /* entry 0 is "Automatic" */
    float scroll;
    char (*files)[PSXS5_PATH_MAX];
    bool unlisted; /* covers/ could not be listed (sandboxed) */
    PlatTexture *preview;
    int preview_for;
} P;

static bool pick_scan(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    while ((e = readdir(d)) && P.count < PICK_MAX)
        if (e->d_name[0] != '.' && covers_is_image(e->d_name))
            path_join(P.files[P.count++], PSXS5_PATH_MAX, dir, e->d_name);
    closedir(d);
    return true;
}

static int pick_compare(const void *a, const void *b)
{
    const char *x = strrchr((const char *)a, '/'), *y = strrchr((const char *)b, '/');
    return str_icmp(x ? x + 1 : (const char *)a, y ? y + 1 : (const char *)b);
}

static void picker_open(int game)
{
    if (!P.files && !(P.files = malloc(sizeof(*P.files) * (PICK_MAX + 1))))
        return;
    P.open = true;
    P.game = game;
    P.count = 1; /* "Automatic" */
    P.cursor = 0;
    P.scroll = 0;
    P.preview_for = -1;
    P.files[0][0] = '\0';
    pick_scan(app.library.games[game].folder);
    /* the covers PSXS5 downloaded for it (flat, 3D, by name) */
    const char *serial = app.library.games[game].serial;
    for (int k = 0; k < 3 && P.count < PICK_MAX; ++k)
    {
        if (k < 2 && !serial[0])
            continue;
        char file[140];
        if (k == 2)
            snprintf(file, sizeof(file), "boxart/%.100s.png", app.library.games[game].id);
        else
            snprintf(file, sizeof(file), k ? "3d/%s.png" : "default/%s.jpg", serial);
        path_join(P.files[P.count], PSXS5_PATH_MAX, app.paths.covers, file);
        FILE *f = fopen(P.files[P.count], "rb");
        if (f)
        {
            fclose(f);
            ++P.count;
        }
    }
    int from = P.count;
    P.unlisted = !pick_scan(app.paths.covers);
    qsort(P.files + from, (size_t)(P.count - from), sizeof(*P.files), pick_compare);
}

static void picker_close(void)
{
    P.open = false;
    plat_texture_free(P.preview);
    P.preview = NULL;
}

static void picker_choose(void)
{
    const Game *g = &app.library.games[P.game];
    char dir[PSXS5_PATH_MAX], path[PSXS5_PATH_MAX], name[96];
    covers_custom_dir(dir, sizeof(dir));
    static const char *const exts[] = {"png", "jpg", "jpeg"};
    for (int i = 0; i < 3; ++i) /* only the copies PSXS5 made itself */
    {
        snprintf(name, sizeof(name), "%s.%s", g->id, exts[i]);
        path_join(path, sizeof(path), dir, name);
        remove(path);
    }
    if (P.cursor > 0)
    {
        char ext[8];
        str_copy(ext, sizeof(ext), path_ext(P.files[P.cursor]));
        for (char *c = ext; *c; ++c)
            *c = (char)(*c | 0x20);
        snprintf(name, sizeof(name), "%s.%s", g->id, ext);
        path_join(path, sizeof(path), dir, name);
        if (!make_dirs(dir) || !file_copy(P.files[P.cursor], path))
        {
            app_toast("Could not save the cover");
            sfx_play(SFX_BACK);
            return;
        }
    }
    covers_reload(P.game);
    app_toast(P.cursor > 0 ? "Cover changed" : "Cover set to automatic");
    sfx_play(SFX_SELECT);
    picker_close();
}

static void picker_input(uint32_t pressed)
{
    int before = P.cursor;
    if (pressed & BIT(BTN_UP))
        --P.cursor;
    if (pressed & BIT(BTN_DOWN))
        ++P.cursor;
    if (pressed & (BIT(BTN_LEFT) | BIT(BTN_L1) | BIT(BTN_L2)))
        P.cursor -= PICK_ROWS;
    if (pressed & (BIT(BTN_RIGHT) | BIT(BTN_R1) | BIT(BTN_R2)))
        P.cursor += PICK_ROWS;
    P.cursor = P.cursor < 0 ? 0 : P.cursor >= P.count ? P.count - 1 : P.cursor;
    if (P.cursor != before)
        sfx_play(SFX_CLICK);
    if (pressed & BIT(BTN_CROSS))
        picker_choose();
    else if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_SQUARE)))
    {
        picker_close();
        sfx_play(SFX_BACK);
    }
}

static void picker_draw(void)
{
    P.t = fminf(fmaxf(P.t + (P.open ? app.dt : -app.dt) * 8.0f, 0.0f), 1.0f);
    if (P.t <= 0.0f || !P.files)
        return;
    float t = P.t, e = 1.0f - (1.0f - t) * (1.0f - t);
    draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xc0000000u, t));
    const float w = 1400, h = 800, x = CENTER_X - w * 0.5f, y = 130 + (1.0f - e) * 40;
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD, t));
    text_draw(x + 40, y + 30, 32, FONT_BOLD, argb_alpha(TH_TEXT, t), ALIGN_LEFT, tr("Choose a cover"));
    text_draw_fit(x + 40, y + 76, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, t), ALIGN_LEFT, 760,
                  app.library.games[P.game].title);

    /* the list */
    const float lx = x + 40, ly = y + 130, lw = 760, rh = 50;
    float target = fminf((float)(P.cursor - PICK_ROWS / 2), (float)(P.count - PICK_ROWS));
    anim_approach(&P.scroll, fmaxf(target, 0.0f), app.dt, TH_SNAP);
    int first = (int)P.scroll;
    for (int i = first; i < P.count && i <= first + PICK_ROWS; ++i)
    {
        float ry = ly + (i - P.scroll) * rh;
        if (ry < ly - rh * 0.5f || ry > ly + rh * (PICK_ROWS - 0.5f))
            continue;
        bool on = i == P.cursor;
        if (on)
            draw_rrect(lx, ry, lw, rh - 6, TH_RADIUS_SMALL, argb_alpha(TH_ROW_SELECTED, t));
        const char *label = tr("Automatic (by serial or name)");
        int icon = ICON_REFRESH;
        if (i > 0)
        {
            const char *slash = strrchr(P.files[i], '/');
            label = slash ? slash + 1 : P.files[i];
            icon = strncmp(P.files[i], app.paths.covers, strlen(app.paths.covers)) == 0 ? ICON_CARDS
                                                                                        : ICON_FOLDER;
        }
        icon_draw(icon, lx + 14, ry + 8, 28, argb_alpha(on ? TH_FOCUS : TH_TEXT_DIM, t));
        text_draw_fit(lx + 56, ry + 10, 22, on ? FONT_BOLD : FONT_REGULAR,
                      argb_alpha(on ? TH_TEXT : TH_TEXT_SOFT, t), ALIGN_LEFT, lw - 72, label);
    }
    if (P.count == 1)
        text_draw_fit(lx, ly + rh * 1.5f, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, t), ALIGN_LEFT, lw,
                      tr(P.unlisted ? "Can't list the covers folder: unlock /data in Settings, System"
                                    : "No images yet: put .png or .jpg files in /data/PSXS5/covers/"));

    /* the preview, decoded once per selected file */
    const float px = x + 860, py = y + 130, pw = 500, ph = 560;
    draw_rrect(px, py, pw, ph, TH_RADIUS_SMALL, argb_alpha(TH_PILL, t));
    if (P.open && P.preview_for != P.cursor)
    {
        plat_texture_free(P.preview);
        P.preview = NULL;
        P.preview_for = P.cursor;
        int iw, ih;
        uint8_t *rgba = P.cursor > 0 ? covers_decode(P.files[P.cursor], &iw, &ih) : NULL;
        if (rgba)
        {
            P.preview = plat_texture_create(rgba, iw, ih, true);
            free(rgba);
        }
    }
    PlatTexture *shown = P.cursor > 0 ? P.preview : covers_get(P.game);
    if (shown)
    {
        int iw, ih;
        plat_texture_size(shown, &iw, &ih);
        float k = fminf((pw - 40) / iw, (ph - 40) / ih);
        plat_draw_texture(shown, px + (pw - iw * k) * 0.5f, py + (ph - ih * k) * 0.5f, iw * k, ih * k,
                          argb_alpha(0xffffffffu, t), true);
    }
    else if (P.cursor > 0)
        text_draw(px + pw * 0.5f, py + ph * 0.5f - 12, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, t),
                  ALIGN_CENTER, tr("Can't read this image"));
    text_draw_fit(x + 40, y + h - 56, 20, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, t), ALIGN_LEFT, w - 80,
                  tr("Images from /data/PSXS5/covers/ and the game's folder. Named like the game, they are used without picking."));
}

/* ---------------------------------------------------------------- grid layout */

#define GRID_COLS 5
#define GRID_ROWS 3

/* Memory Card: covers in a grid on the left, the chosen game's card on the right */
static void draw_grid(int game, float launch)
{
    const float x0 = TH_MARGIN + 8, y0 = 170, tile = 212, gap = 22, title_h = 40;
    const float cell_h = tile + title_h + gap;
    /* scroll by rows, keeping the cursor's row in view */
    static float scroll;
    int row = S.cursor / GRID_COLS;
    static int top_row;
    if (row < top_row)
        top_row = row;
    if (row >= top_row + GRID_ROWS)
        top_row = row - GRID_ROWS + 1;
    anim_approach(&scroll, (float)top_row, app.dt, TH_SNAP);
    plat_set_clip(0, (int)y0 - 16, (int)(x0 + GRID_COLS * (tile + gap)), (int)(GRID_ROWS * cell_h + 8));
    int first = (int)floorf(scroll) * GRID_COLS, last = first + (GRID_ROWS + 1) * GRID_COLS;
    for (int k = first; k < last && k < S.view_count; ++k)
    {
        int c = k % GRID_COLS, r = k / GRID_COLS;
        float x = x0 + c * (tile + gap), y = y0 + (r - scroll) * cell_h;
        bool on = k == S.cursor;
        int index = S.view[k];
        const Game *g = &app.library.games[index];
        draw_rrect(x, y, tile, tile, TH_RADIUS_SMALL, TH_CARD);
        PlatTexture *t = covers_get(index);
        if (t)
        {
            int tw, th;
            plat_texture_size(t, &tw, &th);
            float k2 = fminf((tile - 20) / tw, (tile - 20) / th), w = tw * k2, h = th * k2;
            plat_draw_texture(t, x + (tile - w) * 0.5f, y + (tile - h) * 0.5f, w, h, 0xffffffffu, true);
        }
        else
            icon_draw(ICON_DISC, x + tile * 0.5f - 32, y + tile * 0.5f - 32, 64, TH_TEXT_DIM);
        draw_ach_badge(g, x + tile, y + tile, 15);
        if (on)
            draw_rrect_outline(x - 5, y - 5, tile + 10, tile + 10, TH_RADIUS_SMALL + 4, 4, theme.cover_outline);
        text_draw_fit(x + 4, y + tile + 8, 18, on ? FONT_BOLD : FONT_REGULAR, on ? TH_TEXT : TH_TEXT_DIM, ALIGN_LEFT,
                      tile - 8, g->title);
    }
    plat_set_clip(0, 0, 0, 0);

    /* the chosen game's card */
    const Game *g = &app.library.games[game];
    const float cx = x0 + GRID_COLS * (tile + gap) + 40, cw = plat_width() - TH_MARGIN - cx, cy = y0, ch = 760;
    draw_rrect(cx, cy, cw, ch, TH_RADIUS, TH_CARD);
    PlatTexture *t = covers_get(game);
    const float art = 340;
    if (t)
    {
        int tw, th;
        plat_texture_size(t, &tw, &th);
        float k2 = fminf((cw - 64) / tw, art / th), w = tw * k2, h = th * k2;
        plat_draw_texture(t, cx + (cw - w) * 0.5f, cy + 32, w, h, 0xffffffffu, true);
    }
    float ty = cy + 32 + art + 28;
    float a = 1.0f - launch;
    text_draw_fit(cx + 32, ty, 36, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, cw - 64, g->title);
    GameStats *st = stats_get(g->id);
    char played[48] = "", ach[48] = "";
    if (st && st->seconds)
        stats_format_time(st->seconds, played, sizeof(played));
    if (st && st->ach_total > 0 && st->ach_unlocked >= 0)
        snprintf(ach, sizeof(ach), "%d / %d", st->ach_unlocked, st->ach_total);
    const char *labels[4] = {"Serial", "Region", "Played", "Achievements"};
    const char *values[4] = {g->serial[0] ? g->serial : tr("Unknown"), shelf_region_name(g->serial),
                             played[0] ? played : tr("Not yet"), ach[0] ? ach : tr("Unknown")};
    for (int i = 0; i < 4; ++i)
    {
        float ry = ty + 64 + i * 46;
        text_draw(cx + 32, ry, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_LEFT, tr(labels[i]));
        text_draw_fit(cx + cw * 0.45f, ry, 22, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, cw * 0.55f - 32, values[i]);
    }
    if (st && st->favorite)
        icon_draw(ICON_STAR, cx + cw - 64, cy + ch - 64, 36, TH_GOLD);
}

/* ---------------------------------------------------------------- spines layout */

/* Record Shelf: each game a spine (a slice of its cover's art), the chosen one
 * pulled out face-on above the shelf */
static void draw_spines(float launch)
{
    const float spine_w = 52, gap = 8, spine_h = 330, base = CENTER_Y + COVER_H * 0.5f + 8;
    const float face_h = COVER_H * 0.9f;
    PlatTexture *sel = covers_get(S.view[S.cursor]);
    float face_w = face_h * 0.88f;
    if (sel)
    {
        int tw, th;
        plat_texture_size(sel, &tw, &th);
        face_w = face_h * tw / th;
    }
    /* positions relative to the selection, which slides (S.pos) */
    for (int side = -1; side <= 1; side += 2)
        for (int d = 1; d <= 14; ++d)
        {
            int k = S.cursor + d * side;
            if (k < 0 || k >= S.view_count)
                continue;
            float off = (k - S.pos);
            float x = CENTER_X + side * (face_w * 0.5f + 24) + (off - side) * (spine_w + gap) - (side < 0 ? spine_w : 0);
            if (x + spine_w < 0 || x > plat_width())
                continue;
            int index = S.view[k];
            PlatTexture *t = covers_get(index);
            float y = base - spine_h;
            uint32_t shade = d > 8 ? 0xff9a9a9au : 0xffd8d8d8u;
            if (t)
            {
                /* a slice from the middle of the art, as a spine */
                int tw, th;
                plat_texture_size(t, &tw, &th);
                int sw = tw / 9 > 1 ? tw / 9 : 1;
                plat_draw_texture_region(t, tw / 2 - sw / 2, 0, sw, th, x, y, spine_w, spine_h, shade);
            }
            else
                draw_rrect(x, y, spine_w, spine_h, 3, TH_CARD);
            draw_rect(x + spine_w - 3, y, 3, spine_h, 0x40000000u); /* the spine's edge */
        }
    /* the chosen game, face-on and lifted */
    float lift = 18.0f + launch * 20.0f;
    float fx = CENTER_X - face_w * 0.5f, fy = base - face_h - lift;
    draw_rrect(fx + 10, fy + 24, face_w, face_h, 8, 0x70000000u); /* shadow */
    if (sel)
        plat_draw_texture(sel, fx, fy, face_w, face_h, 0xffffffffu, true);
    else
        draw_rrect(fx, fy, face_w, face_h, 8, TH_CARD);
    draw_rrect_outline(fx - 5, fy - 5, face_w + 10, face_h + 10, 10, 3, theme.cover_outline);
    draw_ach_badge(&app.library.games[S.view[S.cursor]], fx + face_w, fy + face_h, 20);
}

/* ---------------------------------------------------------------- disc animation */

/* A PS1 disc: black rim, the cover's art as its printed label (turning with
 * `spin`), the clear inner ring and the hole. */
static void draw_disc(int game, float cx, float cy, float r, float spin)
{
    draw_circle(cx + r * 0.05f, cy + r * 0.08f, r, 0x70000000u); /* shadow */
    draw_circle(cx, cy, r, 0xff16161cu);
    float lr = r * 0.95f;
    PlatTexture *t = covers_get(game);
    if (t)
    {
        enum { SEG = 72 };
        PlatVertex v[SEG + 2];
        int idx[SEG * 3];
        int tw = 1, th = 1;
        plat_texture_size(t, &tw, &th);
        /* the middle square of the art */
        float ux = tw > th ? (float)th / tw : 1.0f, uy = th > tw ? (float)tw / th : 1.0f;
        v[0] = (PlatVertex){cx, cy, 0.5f, 0.5f, 0xffffffffu};
        for (int i = 0; i <= SEG; ++i)
        {
            float a = 6.2831853f * i / SEG;
            v[i + 1] = (PlatVertex){cx + cosf(a) * lr, cy + sinf(a) * lr, 0.5f + cosf(a - spin) * 0.5f * ux,
                                    0.5f + sinf(a - spin) * 0.5f * uy, 0xffffffffu};
        }
        for (int i = 0; i < SEG; ++i)
        {
            idx[i * 3] = 0;
            idx[i * 3 + 1] = i + 1;
            idx[i * 3 + 2] = i + 2;
        }
        plat_draw_mesh(t, v, SEG + 2, idx, SEG * 3);
    }
    else
        draw_circle(cx, cy, lr, 0xff000000u | (covers_color(game) & 0xffffffu));
    draw_ring(cx, cy, r * 0.97f, 3, 0x50ffffffu);
    draw_circle(cx, cy, r * 0.32f, 0xffc9ced8u); /* the clear plastic ring */
    draw_ring(cx, cy, r * 0.32f, 2, 0x80ffffffu);
    draw_circle(cx, cy, r * 0.12f, 0xff0b0b10u); /* the hole */
}

/* The chosen game's disc slides out of its case (to the right of the cover)
 * and spins up, before the screen fades to the game. */
static void draw_launch_disc(int game, float d, float t)
{
    float ease = 1.0f - (1.0f - d) * (1.0f - d) * (1.0f - d);
    float spin = t * t * 26.0f; /* speeds up */
    if (theme.layout == LAYOUT_GRID)
    {
        /* the grid has no cover in the middle: the disc grows there */
        float r = COVER_H * 0.42f * ease;
        if (r > 2.0f)
            draw_disc(game, CENTER_X, CENTER_Y + 60, r, spin);
        return;
    }
    float r = COVER_H * 0.42f;
    PlatTexture *cover = covers_get(game);
    float aspect = 0.88f;
    if (cover)
    {
        int tw = 1, th = 1;
        plat_texture_size(cover, &tw, &th);
        aspect = (float)tw / th;
    }
    float h = theme.layout == LAYOUT_SPINES ? COVER_H * 0.9f : COVER_H;
    float right = CENTER_X + h * aspect * 0.5f;
    float cx = CENTER_X + (right + r * 0.25f - CENTER_X) * ease;
    /* only the part out of the case shows */
    plat_set_clip((int)right, 0, plat_width() - (int)right, plat_height());
    draw_disc(game, cx, CENTER_Y, r, spin);
    plat_set_clip(0, 0, 0, 0);
}

/* ---------------------------------------------------------------- idle slideshow */

/* After a minute without a button on the shelf: the library's games one by
 * one, full screen (a gameplay picture when there is one, else the cover),
 * slowly drifting. Any button brings the shelf back (and is used up). */
#define IDLE_SECONDS 60.0f
#define SLIDE_SECONDS 7.0f

static struct
{
    float idle, t;
    bool on;
    int game;        /* library index */
    float dir;       /* this slide's drift, -1 or 1 */
} SS;

static void slideshow_next(void)
{
    if (S.view_count <= 0)
        return;
    /* rather a game whose picture is already here (downloaded or loaded) */
    int pick = S.view[rand() % S.view_count];
    for (int tries = 0; tries < 16; ++tries)
    {
        int g = S.view[rand() % S.view_count];
        if (g == SS.game && S.view_count > 1)
            continue;
        pick = g;
        char path[PSXS5_PATH_MAX], file[120];
        snprintf(file, sizeof(file), "art/snaps/%.100s.png", app.library.games[g].id);
        path_join(path, sizeof(path), app.paths.root, file);
        FILE *f = fopen(path, "rb");
        if (f || covers_get(g))
        {
            if (f)
                fclose(f);
            break;
        }
    }
    SS.game = pick;
    SS.t = 0;
    SS.dir = (rand() & 1) ? 1.0f : -1.0f;
    int one[1] = {pick};
    covers_update_view(one, 1, 0); /* load this cover even far from the cursor */
}

/* True while the slideshow has the screen. */
static bool slideshow(uint32_t pressed)
{
    if (pressed || S.launch_t > 0 || S.dialog || P.open)
    {
        bool was = SS.on;
        SS.on = false;
        SS.idle = 0;
        if (was)
            covers_update_view(S.view, S.view_count, S.cursor);
        return was; /* the button that ends it does nothing else */
    }
    SS.idle += app.dt;
    if (!SS.on)
    {
        if (SS.idle < IDLE_SECONDS || S.view_count <= 0)
            return false;
        SS.on = true;
        SS.game = -1;
        slideshow_next();
    }
    SS.t += app.dt;
    if (SS.t > SLIDE_SECONDS)
        slideshow_next();

    const Game *g = &app.library.games[SS.game];
    PlatTexture *pic = art_get(g, ART_SNAP);
    bool snap = pic != NULL;
    if (!pic)
        pic = covers_get(SS.game);
    float fade = fminf(fminf(SS.t / 0.8f, (SLIDE_SECONDS - SS.t) / 0.8f), 1.0f);
    fade = fade < 0 ? 0 : fade;
    float sw = (float)plat_width(), sh = (float)plat_height();
    draw_rect(0, 0, sw, sh, 0xff000000u);
    if (pic)
    {
        int tw, th;
        plat_texture_size(pic, &tw, &th);
        /* gameplay pictures fill the screen; covers are shown whole */
        float k = snap ? fmaxf(sw / tw, sh / th) : fminf(sw * 0.62f / tw, (sh - 380) / th);
        float zoom = 1.04f + 0.06f * SS.t / SLIDE_SECONDS;
        float w = tw * k * zoom, h = th * k * zoom;
        float x = (sw - w) * 0.5f + SS.dir * (SS.t / SLIDE_SECONDS - 0.5f) * sw * 0.04f;
        float y = snap ? (sh - h) * 0.5f : (sh - 250 - h) * 0.5f; /* a cover stays above the title */
        plat_draw_texture(pic, x, y, w, h, argb_alpha(0xffffffffu, fade), true);
    }
    /* the title, bottom left, over a shade */
    for (int i = 0; i < 30; ++i) /* darker towards the bottom */
        draw_rect(0, sh - 300 + i * 10, sw, 10, (uint32_t)(i * 6.5f) << 24);
    text_draw_fit(TH_MARGIN, sh - 170, 56, FONT_BOLD, argb_alpha(0xffffffffu, fade), ALIGN_LEFT, sw - 2 * TH_MARGIN,
                  g->title);
    GameStats *st = stats_get(g->id);
    char line[96] = "";
    if (st && st->seconds)
    {
        char played[48];
        stats_format_time(st->seconds, played, sizeof(played));
        snprintf(line, sizeof(line), tr("Played %s"), played);
    }
    text_draw(TH_MARGIN, sh - 96, 24, FONT_REGULAR, argb_alpha(0xffc8c8d0u, fade), ALIGN_LEFT,
              line[0] ? line : shelf_region_name(g->serial));
    char clock_text[16];
    plat_clock(clock_text, sizeof(clock_text));
    text_draw(sw - TH_MARGIN, 48, 40, FONT_BOLD, 0xd0ffffffu, ALIGN_RIGHT, clock_text);
    text_draw(sw - TH_MARGIN, sh - 96, 22, FONT_REGULAR, 0x90ffffffu, ALIGN_RIGHT, tr("Press any button"));
    return true;
}

void shelf_screen(uint32_t pressed)
{
    if (app.storage_error[0])
    {
        storage_screen(pressed);
        return;
    }
    if (slideshow(pressed))
        return;

    /* ------------------------------------------------ input */
    if (P.open)
    {
        picker_input(pressed);
        pressed = 0;
    }
    if (S.dialog)
    {
        if (pressed & (BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_UP) | BIT(BTN_DOWN)))
        {
            S.dialog_choice ^= 1;
            sfx_play(SFX_CLICK);
        }
        if (pressed & BIT(BTN_CROSS))
        {
            S.dialog = false;
            S.resume = S.dialog_choice == 0;
            S.launch_t = 0.0001f;
            sfx_play(SFX_SELECT);
        }
        if (pressed & BIT(BTN_CIRCLE))
        {
            S.dialog = false;
            sfx_play(SFX_BACK);
        }
        pressed = 0;
    }
    if (S.launch_t <= 0.0f)
    {
        int before = S.cursor;
        int row = theme.layout == LAYOUT_GRID ? GRID_COLS : 1;
        if (pressed & BIT(BTN_LEFT))
            --S.cursor;
        if (pressed & BIT(BTN_RIGHT))
            ++S.cursor;
        if (pressed & BIT(BTN_UP))
            S.cursor = S.cursor - row >= 0 ? S.cursor - row : (row > 1 ? S.cursor : S.cursor - 1);
        if (pressed & BIT(BTN_DOWN))
            S.cursor = S.cursor + row < S.view_count ? S.cursor + row : (row > 1 ? S.cursor : S.cursor + 1);
        if (pressed & BIT(BTN_L2))
            S.cursor -= 8;
        if (pressed & BIT(BTN_R2))
            S.cursor += 8;
        S.cursor = S.cursor < 0 ? 0 : S.cursor >= S.view_count ? (S.view_count ? S.view_count - 1 : 0)
                                                                : S.cursor;
        if (S.cursor != before)
        {
            sfx_play(SFX_CLICK);
            S.title_fade = 0.35f;
        }
        if (pressed & BIT(BTN_L1))
            change_category(-1);
        if (pressed & BIT(BTN_R1))
            change_category(1);
        if (pressed & BIT(BTN_START))
        {
            int keep = selected_game();
            app.global.sort_mode = (app.global.sort_mode + 1) % SORT_COUNT;
            build_view(keep);
            char msg[96];
            snprintf(msg, sizeof(msg), tr("Sorted by %s"), shelf_sort_name(app.global.sort_mode));
            app_toast(msg);
            sfx_play(SFX_CLICK);
        }
        int game = selected_game();
        if ((pressed & BIT(BTN_R3)) && game >= 0)
        {
            GameStats *st = stats_get(app.library.games[game].id);
            if (st)
            {
                st->favorite = !st->favorite;
                stats_save();
                app_toast(st->favorite ? "Added to favorites" : "Removed from favorites");
                sfx_play(SFX_SELECT);
                if (app.global.shelf_category == CAT_FAVORITES)
                    build_view(game);
            }
        }
        if ((pressed & BIT(BTN_L3)) && S.details && game >= 0)
        {
            /* hide / show again: the cursor stays where the game was */
            GameStats *st = stats_get(app.library.games[game].id);
            if (st)
            {
                st->hidden = !st->hidden;
                stats_save();
                app_toast(st->hidden ? "Hidden: find it under Hidden" : "Back on the shelf");
                sfx_play(SFX_SELECT);
                S.details = false;
                int keep_pos = S.cursor;
                build_view(-1);
                S.cursor = keep_pos < S.view_count ? keep_pos : (S.view_count ? S.view_count - 1 : 0);
                S.pos = (float)S.cursor;
                game = selected_game();
            }
        }
        if ((pressed & BIT(BTN_MENU)) && S.view_count > 1 && !S.details)
        {
            /* Surprise me: another game of this category; the shelf glides there */
            int pick = S.cursor;
            while (pick == S.cursor)
                pick = rand() % S.view_count;
            S.cursor = pick;
            S.title_fade = 0.35f;
            app_toast("Surprise!");
            sfx_play(SFX_SELECT);
            game = selected_game();
        }
        if (pressed & BIT(BTN_TRIANGLE) && game >= 0)
        {
            S.details = !S.details;
            sfx_play(S.details ? SFX_SELECT : SFX_BACK);
        }
        if ((pressed & BIT(BTN_CIRCLE)) && S.details)
        {
            S.details = false;
            sfx_play(SFX_BACK);
        }
        if ((pressed & BIT(BTN_CROSS)) && game >= 0)
        {
            const Game *g = &app.library.games[game];
            S.details = false;
            if (app.global.quick_resume && !ra_hardcore() && play_has_resume(g, &S.resume_age))
            {
                /* offer to continue */
                char path[PSXS5_PATH_MAX];
                static uint8_t rgba[THUMB_W * THUMB_H * 4];
                play_resume_path(g, path, sizeof(path));
                plat_texture_free(S.resume_thumb);
                S.resume_thumb = play_load_thumb(path, rgba)
                                     ? plat_texture_create(rgba, THUMB_W, THUMB_H, true)
                                     : NULL;
                S.dialog = true;
                S.dialog_choice = 0;
            }
            else
            {
                S.resume = false;
                S.launch_t = 0.0001f;
            }
            sfx_play(SFX_SELECT);
        }
        if ((pressed & BIT(BTN_SQUARE)) && S.details && game >= 0)
        {
            sfx_play(SFX_SELECT);
            picker_open(game);
        }
        else if (pressed & BIT(BTN_SQUARE))
        {
            sfx_play(SFX_SELECT);
            config_save(&app.global, app.paths.config);
            app_open_settings(SCREEN_LIBRARY);
        }
    }

    /* ------------------------------------------------ animation */
    anim_approach(&S.pos, (float)S.cursor, app.dt, 16.0f);
    S.details_t = fminf(fmaxf(S.details_t + (S.details ? app.dt : -app.dt) * 8.0f, 0.0f), 1.0f);
    if (S.title_fade > 0)
        S.title_fade = fmaxf(S.title_fade - app.dt, 0.0f);
    int game = selected_game();
    if (game >= 0)
        S.last_cursor_game = game;
    if (S.starting)
    {
        /* the last frame (disc and "Loading") stays on screen while the game loads */
        S.starting = false;
        S.launch_t = 0.0f;
        config_save(&app.global, app.paths.config);
        app_start_game(game, S.resume);
    }
    else if (S.launch_t > 0.0f)
    {
        S.launch_t = fminf(S.launch_t + app.dt, launch_total());
        if (S.launch_t >= launch_total())
            S.starting = true;
    }
    plat_profile("start");
    covers_update_view(S.view, S.view_count, S.cursor);
    update_tint(game);
    plat_profile("covers-upload");

    /* ------------------------------------------------ shelf */
    shelf_backdrop();
    if (theme.shelf_plank)
    {
        const float py = CENTER_Y + COVER_H * 0.5f + 8;
        draw_rect(180, py + 30, plat_width() - 360, 22, 0x60000000u);  /* its shadow */
        draw_rrect(160, py, plat_width() - 320, 34, 4, 0xff7a4e2eu);
        draw_rect(160, py + 22, plat_width() - 320, 12, 0xff5e3a22u);
    }
    plat_profile("backdrop");
    /* the fade to black is the last LAUNCH_TIME; the disc animation comes first */
    float launch = S.launch_t > 0.0f ? fmaxf(S.launch_t - (launch_total() - LAUNCH_TIME), 0.0f) / LAUNCH_TIME : 0.0f;
    if (S.view_count > 0 && theme.layout == LAYOUT_GRID)
    {
        draw_grid(game, launch);
        draw_details(&app.library.games[game], S.details_t);
        plat_profile("covers");
    }
    else if (S.view_count > 0 && theme.layout == LAYOUT_SPINES)
    {
        draw_spines(launch);
        plat_profile("covers");
        float text_a = (1.0f - launch) * (1.0f - S.title_fade / 0.35f * 0.8f);
        draw_info(&app.library.games[game], text_a);
        draw_details(&app.library.games[game], S.details_t);
        plat_profile("info");
    }
    else if (S.view_count > 0)
    {
        int first = (int)floorf(S.pos) - 6, last = (int)ceilf(S.pos) + 6;
        /* far to near, so the selected cover is drawn last */
        for (int ring = 6; ring >= 0; --ring)
            for (int side = -1; side <= 1; side += 2)
            {
                int k = (int)floorf(S.pos + 0.5f) + ring * side;
                if (ring == 0 && side == 1)
                    continue;
                if (k < first || k > last || k < 0 || k >= S.view_count)
                    continue;
                float d = k - S.pos, ad = fabsf(d), near = ad < 1.0f ? ad : 1.0f;
                float ox = ad < 1.0f ? d * SIDE_GAP
                                     : (d > 0 ? 1.0f : -1.0f) * (SIDE_GAP + (ad - 1.0f) * STACK_GAP);
                float scale = 1.0f - near * (1.0f - SIDE_SCALE);
                float squeeze = 1.0f - near * (1.0f - SIDE_SQUEEZE);
                bool selected = k == S.cursor;
                if (selected)
                    scale *= 1.0f + launch * 0.12f;
                float fade = ad > 4.0f ? fmaxf(0.0f, 1.0f - (ad - 4.0f) / 2.0f) : 1.0f;
                float shade = (1.0f - near * 0.4f) * fade * (selected ? 1.0f : 1.0f - launch);
                uint8_t s8 = (uint8_t)(255 * shade);
                uint32_t tint = 0xff000000u | (uint32_t)s8 << 16 | (uint32_t)s8 << 8 | s8;
                int index = S.view[k];
                draw_cover(covers_get(index), &app.library.games[index], CENTER_X + ox, CENTER_Y,
                           COVER_H * scale, squeeze, tint, selected && ad < 0.25f && launch < 0.5f);
            }
        plat_profile("covers");
        float text_a = (1.0f - launch) * (1.0f - S.title_fade / 0.35f * 0.8f);
        draw_info(&app.library.games[game], text_a);
        draw_details(&app.library.games[game], S.details_t);
        plat_profile("info");
    }
    else
    {
        float w = 1200, h = 330, x = CENTER_X - w * 0.5f, y = 300;
        draw_rrect(x, y, w, h, TH_RADIUS, TH_CARD_SOFT);
        icon_draw(ICON_DISC, CENTER_X - 32, y + 30, 64, TH_FOCUS);
        text_draw(CENTER_X, y + 108, 36, FONT_BOLD, TH_TEXT, ALIGN_CENTER, tr("Your shelf is empty"));
        char line[PSXS5_PATH_MAX + 64];
        snprintf(line, sizeof(line), tr("Games: %s/<Game name>/"), app.paths.games);
        text_draw(CENTER_X, y + 166, 24, FONT_REGULAR, TH_TEXT, ALIGN_CENTER, line);
        snprintf(line, sizeof(line), tr("BIOS (optional): %s/"), app.paths.bios);
        text_draw(CENTER_X, y + 206, 24, FONT_REGULAR, TH_TEXT, ALIGN_CENTER, line);
        if (app.sandboxed)
        {
            /* games copied another way can't be found without listing /data */
            text_draw(CENTER_X, y + 252, 22, FONT_REGULAR, TH_GOLD, ALIGN_CENTER,
                      tr("Nothing unlocked /data (LegacyJB, etaHEN...), so SwanStationPS5 can't look into the games folder."));
            text_draw(CENTER_X, y + 286, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                      tr("On your PC:  python tools/psxs5_sync.py index --host <PS5 IP>"));
        }
        else
            text_draw(CENTER_X, y + 262, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER,
                      tr("On your PC:  python tools/psxs5_sync.py upload --host <PS5 IP>"));
    }

    draw_header();
    plat_profile("header");
    int pending = covers_downloading();
    if (pending > 0)
    {
        char dl[64];
        snprintf(dl, sizeof(dl), tr("Getting covers (%d)"), pending);
        text_draw(plat_width() - TH_MARGIN, 104, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, dl);
    }
    const int glyphs[] = {GLYPH_CROSS, GLYPH_TRIANGLE, GLYPH_SQUARE, S.details ? GLYPH_L3 : GLYPH_R3,
                          GLYPH_TOUCHPAD};
    GameStats *hint_st = game >= 0 ? stats_get(app.library.games[game].id) : NULL;
    const char *const labels[] = {"Play", "Details", S.details ? "Choose a cover" : "Settings",
                                  S.details ? (hint_st && hint_st->hidden ? "Unhide" : "Hide") : "Favorite",
                                  "Surprise me"};
    char right[128];
    snprintf(right, sizeof(right), "%s   \xc2\xb7   %s: %s", tr("L1 / R1  Category"), tr("OPTIONS  Sort"),
             shelf_sort_name(app.global.sort_mode));
    app_draw_hints(glyphs, labels, S.view_count ? (S.view_count > 1 && !S.details ? 5 : 4) : 1,
                   S.view_count ? right : NULL);
    plat_profile("hints");

    /* the continue dialog */
    S.dialog_t = fminf(fmaxf(S.dialog_t + (S.dialog ? app.dt : -app.dt) * 8.0f, 0.0f), 1.0f);
    if (S.dialog_t > 0.0f && game >= 0)
    {
        float t = S.dialog_t, e = 1.0f - (1.0f - t) * (1.0f - t);
        draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xc0000000u, t));
        const float w = 1000, h = 420, x = CENTER_X - w * 0.5f, y = 330 + (1.0f - e) * 40;
        draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD, t));
        text_draw_fit(x + 40, y + 30, 32, FONT_BOLD, argb_alpha(TH_TEXT, t), ALIGN_LEFT, w - 80,
                      app.library.games[game].title);
        const char *labels[2] = {tr("Continue"), tr("Start over")};
        char age[64], ago[48];
        long a = S.resume_age;
        if (a < 120)
            str_copy(ago, sizeof(ago), "");
        else if (a < 3600)
            snprintf(ago, sizeof(ago), tr("%ld min ago"), a / 60);
        else if (a < 86400)
            snprintf(ago, sizeof(ago), tr("%ld h ago"), a / 3600);
        else
            snprintf(ago, sizeof(ago), tr("%ld days ago"), a / 86400);
        if (ago[0])
            snprintf(age, sizeof(age), tr("Saved %s"), ago);
        else
            str_copy(age, sizeof(age), tr("Saved just now"));
        for (int i = 0; i < 2; ++i)
        {
            float bx = x + 40 + i * (w - 80) * 0.5f, bw = (w - 80) * 0.5f - 12, by = y + 100, bh = 280;
            bool on = i == S.dialog_choice;
            draw_rrect(bx, by, bw, bh, TH_RADIUS_SMALL, argb_alpha(on ? TH_ROW_SELECTED : TH_PILL, t));
            if (on)
                draw_rrect_outline(bx, by, bw, bh, TH_RADIUS_SMALL, 3, argb_alpha(TH_FOCUS, t));
            if (i == 0 && S.resume_thumb)
                plat_draw_texture(S.resume_thumb, bx + (bw - 256) * 0.5f, by + 24, 256, 192,
                                  argb_alpha(0xffffffffu, t), false);
            else
                icon_draw(i == 0 ? ICON_PLAYER_PLAY : ICON_REFRESH, bx + bw * 0.5f - 48, by + 70, 96,
                          argb_alpha(TH_FOCUS, t));
            text_draw(bx + bw * 0.5f, by + 226, 28, FONT_BOLD, argb_alpha(TH_TEXT, t), ALIGN_CENTER,
                      labels[i]);
            if (i == 0)
                text_draw(bx + bw * 0.5f, by + bh + 14, 20, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, t),
                          ALIGN_CENTER, age);
        }
    }

    picker_draw();
    if (S.launch_t > 0.0f)
    {
        bool disc = app.global.disc_animation && game >= 0;
        /* with the disc, the shelf darkens behind it and the disc keeps spinning
         * while the game loads (no black gap); without it, a fade to black */
        draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xff000000u, disc ? launch * 0.85f : launch));
        if (disc)
            draw_launch_disc(game, fminf(S.launch_t / DISC_TIME, 1.0f), S.launch_t);
        if (launch > 0.0f)
            text_draw(CENTER_X, plat_height() - 120, 30, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, launch),
                      ALIGN_CENTER, tr("Compiling Shaders, please wait..."));
    }
    app_draw_toast();
}
