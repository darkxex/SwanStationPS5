/*
 * SwanStationPS5 - game art downloaded on demand: bezels, title screens, gameplay.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_ART_H
#define SwanStationPS5_ART_H

#include "library.h"
#include "platform/platform.h"

enum ArtKind
{
    ART_BEZEL, /* The Bezel Project: artwork around a 4:3 picture, 1920x1080 */
    ART_SNAP,  /* libretro-thumbnails: a gameplay picture */
    ART_TITLE, /* libretro-thumbnails: the title screen */
    ART_KIND_COUNT
};

/* The game's picture, from <root>/art or downloaded (NULL until it's there,
 * or when there is none). Call it each frame the picture is shown. */
PlatTexture *art_get(const Game *g, int kind);
/* A download for it is waiting or running. */
bool art_pending(const Game *g, int kind);

#endif
