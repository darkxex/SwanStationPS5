/*
 * SwanStationPS5 - tips for a game, shown in the shelf's Details panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_TIPS_H
#define SwanStationPS5_TIPS_H

#include "library.h"

#define TIP_LEN 200

/* Up to `max` tips for the game, translated. Returns how many. */
int tips_for(const Game *g, const Settings *settings, char lines[][TIP_LEN], int max);

#endif
