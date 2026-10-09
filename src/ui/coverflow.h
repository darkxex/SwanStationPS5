/*
 * SwanStationPS5 - the game shelf (home screen).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_COVERFLOW_H
#define SwanStationPS5_COVERFLOW_H

#include "../library.h"

enum ShelfCategory
{
    CAT_ALL,
    CAT_RECENT,
    CAT_FAVORITES,
    CAT_MULTI_DISC,
    CAT_USA,
    CAT_EUROPE,
    CAT_JAPAN,
    CAT_MULTIPLAYER, /* 2 players or more (DuckStation's database) */
    CAT_HIDDEN, /* games hidden from the shelf (only shown when there are some) */
    CAT_COUNT
};

enum SortMode
{
    SORT_TITLE,
    SORT_RECENT,
    SORT_MOST_PLAYED,
    SORT_REGION,
    SORT_COUNT
};

/* The shelf's backdrop (also behind settings): a gradient tinted with the
 * selected game's cover colour. */
void shelf_backdrop(void);
/* Region name from a serial's prefix ("USA", "Europe", "Japan"), translated. */
const char *shelf_region_name(const char *serial);
/* The name shown for a game: DuckStation's when its database knows the disc, else the file or folder name. */
const char *shelf_game_title(const Game *g);
/* "Folder: '/data'", the storage a game is loaded from; "" when unknown. */
void shelf_folder_tag(const Game *g, char *out, size_t size);
const char *shelf_sort_name(int sort);

#endif
