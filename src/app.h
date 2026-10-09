/*
 * PSXS5 - state and helpers shared by the screens (main.c owns them).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSXS5_APP_H
#define PSXS5_APP_H

#include "cheats.h"
#include "library.h"
#include "psxs5.h"

enum Screen
{
    SCREEN_LIBRARY,
    SCREEN_GAME,
    SCREEN_MENU,
    SCREEN_SETTINGS,
    SCREEN_CHEATS,
    SCREEN_ACHIEVEMENTS,
    SCREEN_MEMCARDS,
    SCREEN_STATS,
    SCREEN_MANUAL,
    SCREEN_CHEAT_SEARCH,
    SCREEN_GUIDE,
    SCREEN_PROFILE,
    SCREEN_COUNT
};

typedef struct
{
    Paths paths;
    Settings settings;        /* in effect now: the game's own when it has them */
    Settings global;          /* console-wide (psxs5.ini) */
    bool game_has_own;        /* the running game has its own settings file */
    Library library;
    CheatList cheats;
    const Game *game;         /* running game, NULL on the shelf */
    int game_index;           /* its library index */
    enum Screen screen;
    enum Screen settings_return;
    char storage_error[256];
    bool sandboxed;           /* /data can't be listed: library.txt is the library */
    bool unlock_setting;      /* Settings > System > Unlock /data with etaHEN */
    char sandbox_reason[200];
    float dt;                 /* seconds since the last frame */
    float fps;                /* emulated frames per second */
    PadState pads[PSXS5_MAX_PADS]; /* this frame's controllers, as read (by player) */
    bool quit_requested;      /* leave the main loop and close the app */
    double play_seconds;      /* how long this game has been played since it started */
} App;

extern App app;

void app_toast(const char *message); /* translated, shown for 2.5 s */
void app_toast_for(const char *message, float seconds); /* the same, for a chosen time */
void app_draw_toast(void);
/* The running game behind a menu; dim 255 = full brightness. */
void app_draw_game(uint8_t dim);
void app_state_path(char *out, size_t size, int slot);
void app_describe_bios(char *out, size_t size);
void app_rescan(void);
void app_restart_covers(void);
/* resume: continue from the quick-resume save */
void app_start_game(int index, bool resume);
void app_stop_game(void);
/* Path of a game's own settings file. */
void app_game_config_path(char *out, size_t size, const Game *g);
/* Saves psxs5.ini and, when it has them, the running game's own settings. */
void app_save_settings(void);
void app_open_settings(enum Screen back_to);

/* Screens: each handles input and draws one frame. */
void shelf_screen(uint32_t pressed);
void settings_screen(uint32_t pressed);
void menu_screen(uint32_t pressed);
void cheats_screen(uint32_t pressed);
void achievements_screen(uint32_t pressed);
void achievements_open(void);
void memcards_screen(uint32_t pressed);
void memcards_open(enum Screen back_to);
void library_stats_screen(uint32_t pressed);
void library_stats_open(enum Screen back_to);
/* The game's manual: page images in <game folder>/manual/ (0 when none). */
int manual_page_count(void);
void manual_open(void);
void manual_screen(uint32_t pressed);
void cheat_search_open(void);
/* Text guides beside the game (.txt): how many, and the reader. */
int guide_count(void);
void guide_open(void);
void guide_screen(uint32_t pressed);
/* Your RetroAchievements profile. */
void profile_open(enum Screen back_to);
void profile_screen(uint32_t pressed);
void cheat_search_screen(uint32_t pressed);
void menu_open(void);
void settings_opened(void);
void shelf_init(int last_game);
void shelf_select_game(int library_index);
void shelf_library_changed(void); /* after a rescan */

/* Bottom hint bar: pairs of (PadGlyph, label), then an optional right-hand text. */
void app_draw_hints(const int *glyphs, const char *const *labels, int count, const char *right);

#endif
