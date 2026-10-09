/*
 * SwanStationPS5 - interface icons (Tabler Icons subset).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_ICONS_H
#define SwanStationPS5_ICONS_H

#include "../SwanStationPS5.h"
#include "icons_list.h"

/* Draws icon (an ICON_* codepoint) centred in the size x size box at x, y. */
void icon_draw(int icon, float x, float y, float size, uint32_t argb);
void icons_shutdown(void);

#endif
