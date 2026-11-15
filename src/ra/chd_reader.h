/*
 * SwanStationPS5 - reads the sectors of a .chd for RetroAchievements' game hash.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_CHD_READER_H
#define SwanStationPS5_CHD_READER_H

/* Teaches rcheevos' disc hashing to open .chd (through libchdr, which the core already carries), so a .chd hashes the
 * same as the uncompressed disc image. Other images keep rcheevos' own reader. */
void ra_chd_reader_init(void);

#endif
