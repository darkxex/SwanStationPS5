/*
 * SwanStationPS5 - xBR 2x (level 2) pixel-art scaler.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_XBR_H
#define SwanStationPS5_XBR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* src: w x h XRGB8888 with pitch_px; dst: (2w) x (2h), tightly packed. */
void xbr2x(const uint32_t *src, int w, int h, size_t pitch_px, uint32_t *dst);

#endif
