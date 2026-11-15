/*
 * SwanStationPS5 - achievement badges drawn while playing (challenge indicators). The list in the menu has its own.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_BADGES_H
#define SwanStationPS5_BADGES_H

#include "../SwanStationPS5.h"
#include "../platform/platform.h"

/* The badge picture of achievement `id`, or NULL until it is downloaded (the download runs off the main thread;
 * asking again is cheap). Call from the thread that draws. */
PlatTexture *ra_badge(unsigned id, const char *url);
void ra_badges_shutdown(void);

#endif
