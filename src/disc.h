/*
 * SwanStationPS5 - reads a game's serial (SLUS-01041 ...) from its disc image.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DISC_H
#define SwanStationPS5_DISC_H

#include "SwanStationPS5.h"

/* Fills `serial` ("SLUS-01041") from SYSTEM.CNF (.cue/.bin/.iso/.img/.m3u) or
 * PARAM.SFO (.pbp). CHD is compressed and is not read here. */
bool disc_read_serial(const char *path, char *serial, size_t size);

/* Normalises "SLUS_010.41", "SLUS01041", "slus-01041" to "SLUS-01041". */
bool disc_format_serial(const char *raw, char *serial, size_t size);

#endif
