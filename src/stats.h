/*
 * SwanStationPS5 - per-game records: play time, last played, favorite, achievements.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_STATS_H
#define SwanStationPS5_STATS_H

#include "SwanStationPS5.h"

#include <time.h>

typedef struct
{
    char id[64];          /* Game.id */
    uint32_t seconds;     /* total play time */
    int64_t last_played;  /* unix time, 0 = never */
    bool favorite;
    int ach_unlocked;     /* RetroAchievements progress, -1 = unknown */
    int ach_total;
} GameStats;

/* Main-thread API. <root>/stats.txt, one line per game. */
void stats_load(const char *root);
/* Wait for queued writes, then save the current records. */
void stats_save(void);
/* Queue a snapshot, replacing any pending one. Best effort; never waits for disk I/O. */
void stats_save_async(void);
/* Save and join the writer before SDL shuts down. */
void stats_shutdown(void);
/* The record for a game (created on first use; never NULL for a valid id). */
GameStats *stats_get(const char *id);
/* "3 h 12 min", "45 min", "" when never played */
void stats_format_time(uint32_t seconds, char *out, size_t size);
/* "Today", "Yesterday", "3 days ago"...; "" when never */
void stats_format_when(int64_t when, char *out, size_t size);

#endif
