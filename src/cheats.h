/*
 * SwanStationPS5 - RetroArch .cht cheat files (GameShark / Action Replay codes).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_CHEATS_H
#define SwanStationPS5_CHEATS_H

#include "library.h"

#define CHEATS_MAX 512

typedef struct
{
    char desc[80];
    char code[256]; /* "80012345 0063+D0012345 0001" */
    bool enabled;
    bool patch;     /* a patch (widescreen, 60 fps, a fix): listed first */
} Cheat;

typedef struct
{
    Cheat items[CHEATS_MAX];
    int count;
    char source[SwanStationPS5_PATH_MAX]; /* the .cht that was loaded, "" if none */
    char state_path[SwanStationPS5_PATH_MAX];
} CheatList;

/* Finds the best .cht for `game`: one beside the game files first, then the
 * library in `cheats_dir` (libretro-database "Sony - PlayStation" layout).
 * Restores which codes were switched on last time. */
bool cheats_load(CheatList *list, const Game *game, const char *cheats_dir);
void cheats_clear(CheatList *list);
/* True once when a cheat file cheats_load started downloading has arrived:
 * load again then. */
bool cheats_fetch_finished(void);
/* A code of the player's own (Find a code): saved to cheats/mine/<id>.txt,
 * added to the list and switched on. */
bool cheats_add_user(CheatList *list, const Game *game, const char *cheats_dir, const char *desc,
                     const char *code);
/* Two titles with the same words ("Disney-Pixar's ..." = "Disney-Pixar ...",
 * "VII" = "7", "The Legend of X" = "Legend of X, The"). */
bool titles_match(const char *a, const char *b);
/* The best match for the game in a list of Redump-style names (an assets/
 * file, one per line, as cheats-index.txt); false when nothing matches. */
bool cheats_best_in_index(const Game *game, const char *asset, char *name, size_t size);

/* Pushes enabled codes into the running core and remembers the selection. */
void cheats_apply(const CheatList *list);
void cheats_save_selection(const CheatList *list);

#endif
