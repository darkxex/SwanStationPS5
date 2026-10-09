/*
 * SwanStationPS5 - the game's own artwork around a 4:3 picture (The Bezel Project).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_BEZELS_H
#define SwanStationPS5_BEZELS_H

#include "SwanStationPS5.h"

/* Over the game picture, the whole screen; false when there is none (yet:
 * the first time, it is downloaded meanwhile). */
bool bezel_draw(const Settings *view, uint8_t dim);
/* A bezel will be drawn: the border underneath isn't needed. */
bool bezel_shown(const Settings *view);

#endif
