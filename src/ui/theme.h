/*
 * SwanStationPS5 - the interface's colours, sizes and timings, and its themes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The TH_* colours come from the active theme (theme.c), so a theme change
 * shows on the next frame everywhere.
 */
#ifndef SwanStationPS5_THEME_H
#define SwanStationPS5_THEME_H

#include <stdbool.h>
#include <stdint.h>

enum ThemeId
{
    THEME_CLASSIC,     /* deep navy, the 1.1 look */
    THEME_NEON,        /* Neon Arcade */
    THEME_MEMORY,      /* Memory Card, light */
    THEME_MEMORY_DARK, /* Memory Card, dark */
    THEME_RECORD,      /* Record Shelf */
    THEME_COUNT
};

enum ThemeLayout
{
    LAYOUT_FLOW,   /* the cover flow */
    LAYOUT_GRID,   /* a grid of covers and a detail card */
    LAYOUT_SPINES  /* spines on a shelf, the chosen one pulled out */
};

enum ThemeBackdrop
{
    BACKDROP_STAGE, /* a lit band behind the covers, the 1.1 backdrop */
    BACKDROP_GRID,  /* a dark room and a glowing grid floor */
    BACKDROP_FLAT,  /* an even ground, lighter at the top */
    BACKDROP_LAMP   /* a warm pool of light from above */
};

typedef struct
{
    uint32_t bg, bg_deep, card, card_soft, row_selected, focus, pill, switch_on, switch_off;
    uint32_t text, text_dim, text_soft, hint, gold, danger, good, divider;
    uint32_t cover_outline; /* around the selected cover */
    uint32_t backdrop_tint; /* 0: the cover's colour (Classic) */
    int backdrop;           /* enum ThemeBackdrop */
    bool shelf_plank;       /* a wooden shelf under the covers */
    int layout;             /* enum ThemeLayout */
    bool light;             /* dark text on a light ground */
    const char *font_regular, *font_bold; /* assets/fonts/... */
} Theme;

extern Theme theme;
extern const char *const THEME_NAMES[THEME_COUNT];
/* Colours, fonts and backdrop of a theme; reloads the fonts when they change. */
void theme_apply(int id);
int theme_current(void);

/* colours, ARGB */
#define TH_BG (theme.bg)                 /* screen background */
#define TH_BG_DEEP (theme.bg_deep)
#define TH_CARD (theme.card)             /* grouped settings, panels */
#define TH_CARD_SOFT (theme.card_soft)   /* panels over the game */
#define TH_ROW_SELECTED (theme.row_selected)
#define TH_FOCUS (theme.focus)           /* focus outline, accents */
#define TH_PILL (theme.pill)
#define TH_SWITCH_ON (theme.switch_on)
#define TH_SWITCH_OFF (theme.switch_off)
#define TH_TEXT (theme.text)
#define TH_TEXT_DIM (theme.text_dim)
#define TH_TEXT_SOFT (theme.text_soft)
#define TH_HINT (theme.hint)
#define TH_GOLD (theme.gold)
#define TH_DANGER (theme.danger)
#define TH_GOOD (theme.good)
#define TH_DIVIDER (theme.divider)
/* the card or pill colour with another alpha (0..255) */
#define TH_CARD_A(a) (((uint32_t)(a) << 24) | (theme.card & 0xffffffu))
#define TH_PILL_A(a) (((uint32_t)(a) << 24) | (theme.pill & 0xffffffu))
#define TH_BG_A(a) (((uint32_t)(a) << 24) | (theme.bg & 0xffffffu))
/* what dims the game behind the in-game screens: dark, or light for light themes */
#define TH_SCRIM (theme.light ? TH_BG_A(0xd8) : (0x90000000u | (theme.bg_deep & 0xffffffu)))

/* sizes, in 1920x1080 screen pixels */
#define TH_RADIUS 16.0f
#define TH_RADIUS_SMALL 12.0f
#define TH_MARGIN 64.0f
#define TH_HINT_Y 1012.0f

/* animation: how quickly highlights and panels catch up (per second) */
#define TH_SNAP 22.0f

#endif
