/*
 * SwanStationPS5 - state and helpers shared by the screens (main.c owns them).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_APP_H
#define SwanStationPS5_APP_H

#include "cheats.h"
#include "library.h"
#include "SwanStationPS5.h"

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
    SCREEN_RA_LOGIN,
    SCREEN_TEXT_INPUT,
    SCREEN_COUNT
};

typedef struct
{
    Paths paths;
    Settings settings;        /* in effect now: the game's own when it has them */
    Settings global;          /* console-wide (SwanStationPS5.ini) */
    bool game_has_own;        /* the running game has its own settings file */
    Library library;
    CheatList cheats;
    const Game *game;         /* running game, NULL on the shelf */
    const Game *settings_game; /* the shelf's game whose own settings Settings edits (opened from the shelf), else NULL */
    int game_index;           /* its library index */
    enum Screen screen;
    enum Screen settings_return;
    bool settings_preview;    /* Settings opened from a game: the touchpad hides it and the game runs, to see the changes live */
    float settings_preview_time; /* seconds since the preview began (its notice fades out) */
    char storage_error[256];
    char drive_text[10][40]; /* the drives found when the app started, with their free space */
    int drive_count;
    bool sandboxed;           /* /data can't be listed: library.txt is the library */
    bool unlock_setting;      /* Settings > System > Unlock /data with etaHEN */
    char sandbox_reason[200];
    float dt;                 /* seconds since the last frame */
    float fps;                /* emulated frames per second */
    float real_fps;           /* and the game's own among them (a 30 fps game repeats each picture on a 60 fps core) */
    PadState pads[SwanStationPS5_MAX_PADS]; /* this frame's controllers, as read (by player) */
    bool quit_requested;      /* leave the main loop and close the app */
    double play_seconds;      /* how long this game has been played since it started */
} App;

extern App app;

void app_toast(const char *message); /* translated, shown for 2.5 s */
void app_nfs_notify(void); /* the next NFS mount says in a notice how it went */
void app_toast_for(const char *message, float seconds); /* the same, for a chosen time */
void app_draw_toast(void);
/* A message in a square box in the middle of the screen, over whatever screen is up. It stays until X is pressed. */
void app_message_box(const char *message);
/* The running game behind a menu; dim 255 = full brightness. */
void app_draw_game(uint8_t dim);
void app_state_path(char *out, size_t size, int slot);
void app_describe_bios(char *out, size_t size);
void app_rescan(void);
void app_restart_covers(void);
/* The controller that moves through the app (the shelf, Settings, dialogs): player 1's; the others are only for playing.
 * If player 1's isn't connected, the first one that is, so the console is never left without a way to move. */
int app_nav_pad(void);
/* resume: continue from the quick-resume save */
void app_start_game(int index, bool resume);
/* X on a game that stands for a disc in the drive not in the library yet: asks about it again (true: nothing else starts) */
bool app_disc_pending(int index);
/* Boots the console's BIOS with no disc (Settings > About > Start BIOS) */
void app_start_bios(void);
void app_stop_game(void);
/* Path of a game's own settings file. */
void app_game_config_path(char *out, size_t size, const Game *g);
/* Saves SwanStationPS5.ini and, when it has them, the running game's own settings. */
void app_save_settings(void);
void app_open_settings(enum Screen back_to);
/* Settings from the shelf, with `game` (the one under the cursor) to choose "This game" for. */
void app_open_shelf_settings(const Game *game);
/* Back from Settings to the shelf: the shelf's game no longer has its settings loaded. */
void app_close_shelf_settings(void);

/* Screens: each handles input and draws one frame. */
void shelf_screen(uint32_t pressed);
void settings_screen(uint32_t pressed);
/* Manual crop (Settings > Display): changes the picture's width / height (percent) and position (thousandths of the
 * screen) in the settings the row edits; reset puts it back as it was drawn by default. */
void manual_crop_edit(int grow, int dy, bool reset);
void manual_crop_fix(void); /* the picture's area keeps the aspect ratio and is not moved sideways */
bool settings_preview_allowed(void); /* the main settings view (not the button-mapping, players or profiles pages) */
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
/* A line of text typed with the on-screen keyboard; Apply hands it to `done`, Back leaves it as it was. */
typedef void (*TextInputDone)(const char *text);
void textinput_open(enum Screen back_to, const char *title, const char *prompt, const char *initial,
                    TextInputDone done);
void textinput_screen(uint32_t pressed);
void login_open(enum Screen back_to);   /* RetroAchievements sign-in, typed on the console */
void login_screen(uint32_t pressed);
void profile_screen(uint32_t pressed);
void cheat_search_screen(uint32_t pressed);
void menu_open(void);
void settings_opened(void);
void shelf_init(int last_game);
void shelf_select_game(int library_index);
/* Nothing is open on the shelf (no Continue box, no launch under way, no cover picker). */
bool shelf_idle(void);
/* The picture the shelf shows for a game without a cover (assets/nocover2.png). Not to be freed. */
struct PlatTexture *shelf_no_cover(void);
/* A game's cover for screens other than the shelf: assets/nocover2.png when it has none. Not to be freed. */
struct PlatTexture *shelf_cover_or_none(int index);
/* Puts the cursor on the game and starts it: a disc put in the drive. When the game has a quick-resume save, the
 * Continue or Start over box comes first. With `direct` (the app was opened by the disc daemon, nobody is watching) and
 * no box, it also skips the opening animation and the click; otherwise it plays as when ✕ is pressed. False if the
 * shelf is busy or the game cannot be shown. */
bool shelf_show_game(int library_index); /* moves the cursor to the game (the shelf shows all if its category hides it) */
bool shelf_play_game(int library_index, bool direct);
/* The same as pressing X on the game: offers Continue or Start over when it has a quick-resume save, else starts it. */
bool shelf_start_game(int library_index);
void shelf_library_changed(void); /* after a rescan */
void shelf_show_all(void);        /* after a rescan the player asked for: the category All, without a filter */

/* Bottom hint bar: pairs of (PadGlyph, label), then an optional right-hand text. */
void app_draw_hints(const int *glyphs, const char *const *labels, int count, const char *right);
/* The same bar's right-hand side, with button icons: each item is glyphs joined by sep, then a label. */
typedef struct
{
    int glyphs[3];
    int count;
    char sep;
    const char *label;
} HintCombo;
void app_draw_hints_right(const HintCombo *items, int count);

#endif
