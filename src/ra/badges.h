/*
 * SwanStationPS5 - achievement badges drawn while playing (challenge indicators). The list in the menu has its own.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_BADGES_H
#define SwanStationPS5_BADGES_H

#include "../SwanStationPS5.h"
#include "../platform/platform.h"
#include "achievements.h"

/* The badge picture of achievement `id`, or NULL until it is downloaded (the download runs off the main thread;
 * asking again is cheap). Call from the thread that draws. */
PlatTexture *ra_badge(unsigned id, const char *url);
/* The badge of achievement `id` as a texture of the caller's (free it with plat_texture_free), only when its picture is
 * on disk already; NULL when it is not. Never downloads. */
PlatTexture *ra_badge_cached(unsigned id);
/* Downloads, one by one off the main thread, the pictures of these achievements that are not on disk (the unlocked one
 * of each, and the locked one of those still locked). A new call or ra_badges_prefetch_stop() ends the one running. */
void ra_badges_prefetch(const RaBadgeJob *jobs, int count);
void ra_badges_prefetch_stop(void);
void ra_badges_shutdown(void);

#endif
