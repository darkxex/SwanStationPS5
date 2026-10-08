/*
 * PSXS5 - Your library: games, play time, achievements and the most played.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../covers.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "../ra/achievements.h"
#include "../stats.h"
#include "coverflow.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>

#define TOP 5

static struct
{
    enum Screen back_to;
    int games, played, favorites, hidden, ach_unlocked, ach_total, games_with_ach;
    uint64_t seconds;
    int top[TOP], top_count;
} L;

static uint32_t seconds_of(int index)
{
    GameStats *st = stats_get(app.library.games[index].id);
    return st ? st->seconds : 0;
}

static void gather(void)
{
    enum Screen back_to = L.back_to;
    memset(&L, 0, sizeof(L));
    L.back_to = back_to;
    L.games = app.library.count;
    for (int i = 0; i < app.library.count; ++i)
    {
        GameStats *st = stats_get(app.library.games[i].id);
        if (!st)
            continue;
        L.played += st->seconds > 0;
        L.favorites += st->favorite;
        L.hidden += st->hidden;
        L.seconds += st->seconds;
        if (st->ach_total > 0 && st->ach_unlocked >= 0)
        {
            L.ach_unlocked += st->ach_unlocked;
            L.ach_total += st->ach_total;
            ++L.games_with_ach;
        }
        /* the most played: kept sorted, longest first */
        if (!st->seconds)
            continue;
        int k;
        if (L.top_count < TOP)
            k = L.top_count++;
        else if (st->seconds > seconds_of(L.top[TOP - 1]))
            k = TOP - 1;
        else
            continue;
        L.top[k] = i;
        for (; k > 0 && seconds_of(L.top[k - 1]) < st->seconds; --k)
        {
            int t = L.top[k - 1];
            L.top[k - 1] = L.top[k];
            L.top[k] = t;
        }
    }
}

void library_stats_open(enum Screen back_to)
{
    L.back_to = back_to;
    gather();
    app.screen = SCREEN_STATS;
}

static void tile(float x, float y, float w, int icon, const char *value, const char *label)
{
    draw_rrect(x, y, w, 150, TH_RADIUS, TH_CARD);
    icon_draw(icon, x + 28, y + 30, 40, TH_FOCUS);
    text_draw_fit(x + 84, y + 26, 40, FONT_BOLD, TH_TEXT, ALIGN_LEFT, w - 104, value);
    text_draw_fit(x + 28, y + 96, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, w - 56, label);
}

void library_stats_screen(uint32_t pressed)
{
    if ((pressed & BIT(BTN_SQUARE)) && ra_user()[0])
    {
        sfx_play(SFX_SELECT);
        profile_open(SCREEN_STATS);
        return;
    }
    if (pressed & (BIT(BTN_CIRCLE) | BIT(BTN_CROSS)))
    {
        sfx_play(SFX_BACK);
        app.screen = L.back_to;
        return;
    }
    covers_update_view(L.top, L.top_count, 0);

    shelf_backdrop();
    text_draw(TH_MARGIN, 40, 44, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Your library"));

    char value[64], label[96];
    const float gap = 24, w = (plat_width() - 2 * TH_MARGIN - 3 * gap) / 4, y = 130;
    snprintf(value, sizeof(value), "%d", L.games);
    snprintf(label, sizeof(label), tr("games, %d played"), L.played);
    tile(TH_MARGIN, y, w, ICON_DISC, value, label);
    stats_format_time((uint32_t)(L.seconds > 0xffffffffu ? 0xffffffffu : L.seconds), value, sizeof(value));
    if (!L.seconds)
        str_copy(value, sizeof(value), "0");
    tile(TH_MARGIN + (w + gap), y, w, ICON_CLOCK, value, tr("played in SwanStationPS5"));
    snprintf(value, sizeof(value), "%d", L.ach_unlocked);
    if (L.ach_total)
        snprintf(label, sizeof(label), tr("of %d achievements, in %d games"), L.ach_total, L.games_with_ach);
    else
        str_copy(label, sizeof(label), tr("achievements"));
    tile(TH_MARGIN + 2 * (w + gap), y, w, ICON_TROPHY, value, label);
    snprintf(value, sizeof(value), "%d", L.favorites);
    if (L.hidden)
        snprintf(label, sizeof(label), tr("favorites, %d hidden"), L.hidden);
    else
        str_copy(label, sizeof(label), tr("favorites"));
    tile(TH_MARGIN + 3 * (w + gap), y, w, ICON_STAR, value, label);

    /* the most played, with their covers */
    const float ty = 330, cw = (plat_width() - 2 * TH_MARGIN - 4 * gap) / TOP;
    text_draw(TH_MARGIN, ty, 28, FONT_BOLD, TH_TEXT, ALIGN_LEFT, tr("Most played"));
    if (!L.top_count)
        text_draw(TH_MARGIN, ty + 60, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, tr("Nothing played yet."));
    for (int k = 0; k < L.top_count; ++k)
    {
        const Game *g = &app.library.games[L.top[k]];
        float x = TH_MARGIN + k * (cw + gap), cy = ty + 56, ch = cw * 0.92f;
        draw_rrect(x, cy, cw, ch + 120, TH_RADIUS, TH_CARD);
        PlatTexture *t = covers_get(L.top[k]);
        if (t)
        {
            int iw, ih;
            plat_texture_size(t, &iw, &ih);
            float s = (ch - 24) / (float)ih, dw = iw * s > cw - 24 ? cw - 24 : iw * s;
            plat_draw_texture(t, x + (cw - dw) * 0.5f, cy + 12, dw, ch - 24, 0xffffffffu, false);
        }
        else
            icon_draw(ICON_DISC, x + cw * 0.5f - 40, cy + ch * 0.5f - 40, 80, TH_TEXT_DIM);
        char rank[8];
        snprintf(rank, sizeof(rank), "%d", k + 1);
        draw_rrect(x + 12, cy + 12, 40, 40, 20, TH_GOLD);
        text_draw(x + 32, cy + 18, 22, FONT_BOLD, TH_BG, ALIGN_CENTER, rank);
        text_draw_fit(x + 16, cy + ch + 12, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, cw - 32, g->title);
        GameStats *st = stats_get(g->id);
        char played[48] = "";
        if (st)
            stats_format_time(st->seconds, played, sizeof(played));
        text_draw(x + 16, cy + ch + 52, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, played);
    }

    static const int glyphs[] = {GLYPH_CIRCLE, GLYPH_SQUARE};
    static const char *const labels[] = {"Back", "RetroAchievements profile"};
    app_draw_hints(glyphs, labels, ra_user()[0] ? 2 : 1, NULL);
    app_draw_toast();
}
