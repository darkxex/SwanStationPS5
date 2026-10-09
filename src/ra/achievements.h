/*
 * SwanStationPS5 - RetroAchievements (retroachievements.org) through rcheevos.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_ACHIEVEMENTS_H
#define SwanStationPS5_ACHIEVEMENTS_H

#include "../SwanStationPS5.h"

/* Reads <root>/retroachievements.ini (user=, token=, hardcore=) written by
 * `tools/SwanStationPS5_sync.py ra-login`, and signs in in the background. */
void ra_init(const Paths *paths);
void ra_shutdown(void);

/* After the core loaded a game: hashes the disc and loads its achievements. */
void ra_game_loaded(const char *game_path);
void ra_game_unloaded(void);
void ra_frame(void);  /* after every emulated frame */
void ra_idle(void);   /* while paused (menus): keeps the server session alive */
void ra_reset(void);  /* the console was reset */

bool ra_signed_in(void);
const char *ra_user(void);
unsigned ra_user_score(void); /* points, 0 when not signed in */
/* The loaded game's achievements; false when it has none or none loaded. */
bool ra_game_progress(int *unlocked, int *total);

typedef struct
{
    char title[96];
    char description[192];
    char progress[24];   /* "3/10" for counted ones, else "" */
    float percent;       /* 0..100 for counted ones */
    char badge_url[160]; /* the badge picture (locked or unlocked) */
    unsigned points, id;
    bool unlocked;
} RaAchievement;
/* The loaded game's achievements, unlocked ones first. Returns how many. */
int ra_list(RaAchievement *out, int max);
/* The unlocks SwanStationPS5 has seen (its own log), newest first; max <= 64. */
typedef struct
{
    long long when;
    unsigned points;
    char game[96], title[96], description[192];
} RaRecent;
int ra_recent(RaRecent *out, int max);
unsigned ra_user_hardcore_score(void);
unsigned ra_user_softcore_score(void);
/* Signs in with a token got elsewhere (the phone page), or out; saved per profile. */
void ra_use_token(const char *user, const char *token);
void ra_sign_out(void);
bool ra_hardcore(void);          /* blocks save states, cheats and rewind */
void ra_set_hardcore(bool on);   /* saved to the ini */
/* "12 of 40 achievements, 115 of 400 points", or "" when no set is loaded */
void ra_game_summary(char *out, size_t size);

/* Messages for the UI (unlocks, leaderboards...). Returns false when empty. */
bool ra_next_message(char *title, size_t title_size, char *detail, size_t detail_size);

/* While playing: the counted achievement that just moved (18/80 dragons).
 * False when there's nothing to show. */
bool ra_tracker(char *title, size_t title_size, char *progress, size_t progress_size, float *percent);

#endif
