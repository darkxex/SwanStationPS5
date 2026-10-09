/*
 * SwanStationPS5 - the game's own artwork around a 4:3 picture (Settings > Display >
 * Game artwork border), from The Bezel Project (art.c downloads it once into
 * <root>/art/bezels). Each is a 1920x1080 picture with a clear 4:3 window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bezels.h"

#include "app.h"
#include "art.h"
#include "platform/platform.h"

static bool wanted(const Settings *view)
{
    return view->bezel && app.game && view->aspect != ASPECT_STRETCH && view->aspect != ASPECT_16_9 &&
           view->aspect != ASPECT_16_10;
}

bool bezel_draw(const Settings *view, uint8_t dim)
{
    PlatTexture *t = wanted(view) ? art_get(app.game, ART_BEZEL) : NULL;
    if (!t)
        return false;
    plat_draw_texture(t, 0, 0, (float)plat_width(), (float)plat_height(),
                      0xff000000u | (uint32_t)dim << 16 | (uint32_t)dim << 8 | dim, true);
    return true;
}

bool bezel_shown(const Settings *view)
{
    return wanted(view) && art_get(app.game, ART_BEZEL) != NULL;
}
