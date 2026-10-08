/*
 * PSXS5 - your RetroAchievements profile: points, games mastered, what you
 * unlocked lately and the games closest to mastery.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The points come from the server (sign-in); the rest from what PSXS5 keeps:
 * stats.txt (each game's progress, saved when you quit it) and its log of
 * unlocks. The avatar is downloaded once to <root>/art/avatar-<user>.png.
 */
#include "../app.h"
#include "../i18n.h"
#include "../net.h"
#include "../platform/platform.h"
#include "../ra/achievements.h"
#include "../stats.h"
#include "coverflow.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "stb_image.h"
#include "text.h"
#include "theme.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#define RECENT 8
#define CLOSEST 6

static struct
{
    enum Screen back_to;
    int earned, total, mastered, started;
    RaRecent recent[RECENT];
    int recent_count;
    int closest[CLOSEST], closest_count;
    PlatTexture *avatar;
    bool avatar_tried;
} P;

static SDL_atomic_t avatar_state; /* 0 idle, 1 downloading, 2 arrived */
static char avatar_url[256], avatar_file[PSXS5_PATH_MAX];

static int fetch_avatar(void *unused)
{
    (void)unused;
    char temp[PSXS5_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.part", avatar_file);
    bool ok = net_download(avatar_url, temp) == NET_OK && rename(temp, avatar_file) == 0;
    if (!ok)
        remove(temp);
    SDL_AtomicSet(&avatar_state, ok ? 2 : 0);
    return 0;
}

static float percent_of(int index)
{
    GameStats *st = stats_get(app.library.games[index].id);
    return st && st->ach_total > 0 && st->ach_unlocked >= 0 ? (float)st->ach_unlocked / st->ach_total : 0.0f;
}

static void gather(void)
{
    P.earned = P.total = P.mastered = P.started = 0;
    P.closest_count = 0;
    for (int i = 0; i < app.library.count; ++i)
    {
        GameStats *st = stats_get(app.library.games[i].id);
        if (!st || st->ach_total <= 0 || st->ach_unlocked < 0)
            continue;
        P.earned += st->ach_unlocked;
        P.total += st->ach_total;
        P.started += st->ach_unlocked > 0;
        if (st->ach_unlocked >= st->ach_total)
        {
            ++P.mastered;
            continue;
        }
        if (st->ach_unlocked == 0)
            continue;
        /* the closest to mastery, highest share first */
        int k;
        float p = percent_of(i);
        if (P.closest_count < CLOSEST)
            k = P.closest_count++;
        else if (p > percent_of(P.closest[CLOSEST - 1]))
            k = CLOSEST - 1;
        else
            continue;
        P.closest[k] = i;
        for (; k > 0 && percent_of(P.closest[k - 1]) < p; --k)
        {
            int t = P.closest[k - 1];
            P.closest[k - 1] = P.closest[k];
            P.closest[k] = t;
        }
    }
    P.recent_count = ra_recent(P.recent, RECENT);
}

void profile_open(enum Screen back_to)
{
    if (!ra_user()[0])
    {
        app_toast("Sign in first: psxs5_sync.py ra-login");
        return;
    }
    P.back_to = back_to;
    gather();
    if (!P.avatar_tried)
    {
        P.avatar_tried = true;
        char dir[PSXS5_PATH_MAX], file[160];
        path_join(dir, sizeof(dir), app.paths.root, "art");
        make_dirs(dir);
        snprintf(file, sizeof(file), "avatar-%.60s.png", ra_user());
        path_join(avatar_file, sizeof(avatar_file), dir, file);
        FILE *f = fopen(avatar_file, "rb");
        if (f)
        {
            fclose(f);
            SDL_AtomicSet(&avatar_state, 2);
        }
        else if (net_available() && SDL_AtomicCAS(&avatar_state, 0, 1))
        {
            snprintf(avatar_url, sizeof(avatar_url), "https://media.retroachievements.org/UserPic/%s.png", ra_user());
            SDL_Thread *t = SDL_CreateThread(fetch_avatar, "avatar", NULL);
            if (t)
                SDL_DetachThread(t);
            else
                SDL_AtomicSet(&avatar_state, 0);
        }
    }
    app.screen = SCREEN_PROFILE;
}

static void tile(float x, float y, float w, int icon, uint32_t tint, const char *value, const char *label)
{
    draw_rrect(x, y, w, 140, TH_RADIUS, TH_CARD);
    icon_draw(icon, x + 28, y + 28, 38, tint);
    text_draw_fit(x + 80, y + 22, 40, FONT_BOLD, TH_TEXT, ALIGN_LEFT, w - 100, value);
    text_draw_fit(x + 28, y + 88, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w - 56, label);
}

void profile_screen(uint32_t pressed)
{
    if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_CROSS)))
    {
        sfx_play(SFX_BACK);
        app.screen = P.back_to;
        return;
    }
    if (SDL_AtomicCAS(&avatar_state, 2, 3) && !P.avatar)
    {
        int w, h, n;
        uint8_t *rgba = stbi_load(avatar_file, &w, &h, &n, 4);
        if (rgba)
        {
            P.avatar = plat_texture_create(rgba, w, h, true);
            stbi_image_free(rgba);
        }
    }

    if (app.game)
    {
        app_draw_game(40);
        draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);
    }
    else
        shelf_backdrop();

    /* who */
    const float ax = TH_MARGIN, ay = 40, as = 120;
    draw_rrect(ax, ay, as, as, TH_RADIUS, TH_CARD);
    if (P.avatar)
        plat_draw_texture(P.avatar, ax, ay, as, as, 0xffffffffu, true);
    else
        icon_draw(ICON_TROPHY, ax + 30, ay + 30, 60, TH_GOLD);
    text_draw(ax + as + 32, ay + 8, 48, FONT_BOLD, TH_TEXT, ALIGN_LEFT, ra_user());
    char points[128];
    snprintf(points, sizeof(points), tr("%u points (hardcore)   %u points (softcore)"), ra_user_hardcore_score(),
             ra_user_softcore_score());
    text_draw(ax + as + 32, ay + 74, 24, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT,
              ra_signed_in() ? points : tr("Not signed in yet: the points show once SwanStationPS5 is online"));

    /* the numbers */
    char value[48], label[96];
    const float gap = 24, w = (plat_width() - 2 * TH_MARGIN - 3 * gap) / 4, y = 200;
    snprintf(value, sizeof(value), "%d", P.earned);
    snprintf(label, sizeof(label), tr("of %d achievements"), P.total);
    tile(TH_MARGIN, y, w, ICON_TROPHY, TH_GOLD, value, P.total ? label : tr("achievements"));
    snprintf(value, sizeof(value), "%d", P.mastered);
    tile(TH_MARGIN + (w + gap), y, w, ICON_STAR, TH_GOLD, value, tr("games mastered"));
    snprintf(value, sizeof(value), "%d", P.started);
    tile(TH_MARGIN + 2 * (w + gap), y, w, ICON_DEVICE_GAMEPAD_2, TH_FOCUS, value, tr("games with an unlock"));
    snprintf(value, sizeof(value), "%d%%", P.total ? P.earned * 100 / P.total : 0);
    tile(TH_MARGIN + 3 * (w + gap), y, w, ICON_SPARKLES, TH_FOCUS, value, tr("of their achievements"));

    /* recently unlocked, on the left */
    const float cy = 380, col = (plat_width() - 2 * TH_MARGIN - gap) / 2;
    float lx = TH_MARGIN, rx = TH_MARGIN + col + gap;
    draw_rrect(lx, cy, col, 590, TH_RADIUS, TH_CARD);
    text_draw(lx + 28, cy + 22, 26, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Recently unlocked"));
    if (!P.recent_count)
        text_draw_fit(lx + 28, cy + 80, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, col - 56,
                      tr("Unlocks earned in SwanStationPS5 show here."));
    for (int i = 0; i < P.recent_count; ++i)
    {
        const RaRecent *r = &P.recent[i];
        float ry = cy + 74 + i * 64;
        icon_draw(ICON_TROPHY, lx + 28, ry + 6, 30, TH_GOLD);
        text_draw_fit(lx + 76, ry, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, col - 260, r->title);
        char when[48] = "";
        stats_format_when(r->when, when, sizeof(when));
        text_draw(lx + col - 28, ry, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, when);
        char sub[160];
        snprintf(sub, sizeof(sub), tr("%s  \xc2\xb7  %u points"), r->game, r->points);
        text_draw_fit(lx + 76, ry + 30, 19, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, col - 110, sub);
    }

    /* closest to mastery, on the right */
    draw_rrect(rx, cy, col, 590, TH_RADIUS, TH_CARD);
    text_draw(rx + 28, cy + 22, 26, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Closest to mastery"));
    if (!P.closest_count)
        text_draw_fit(rx + 28, cy + 80, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, col - 56,
                      tr("Play a game with achievements to see its progress here."));
    for (int i = 0; i < P.closest_count; ++i)
    {
        const Game *g = &app.library.games[P.closest[i]];
        GameStats *st = stats_get(g->id);
        float ry = cy + 78 + i * 84, p = percent_of(P.closest[i]);
        text_draw_fit(rx + 28, ry, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, col - 200, g->title);
        char n[32];
        snprintf(n, sizeof(n), "%d / %d", st ? st->ach_unlocked : 0, st ? st->ach_total : 0);
        text_draw(rx + col - 28, ry, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_RIGHT, n);
        draw_rrect(rx + 28, ry + 40, col - 56, 12, 6, TH_PILL);
        draw_rrect(rx + 28, ry + 40, (col - 56) * p, 12, 6, TH_GOLD);
    }

    static const int glyphs[] = {GLYPH_CIRCLE};
    static const char *const labels[] = {"Back"};
    app_draw_hints(glyphs, labels, 1, NULL);
    app_draw_toast();
}
