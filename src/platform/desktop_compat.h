/*
 * SwanStationPS5 desktop build - what the Windows (mingw) C library lacks. Force-included by CMakeLists.txt.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DESKTOP_COMPAT_H
#define SwanStationPS5_DESKTOP_COMPAT_H
#if defined(_WIN32) && defined(SwanStationPS5_DESKTOP)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <time.h>
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define mkdir(path, mode) _mkdir(path)
static inline struct tm *SwanStationPS5_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : 0;
}
#define localtime_r SwanStationPS5_localtime_r
/* Windows' rename() fails when the target exists, and the app saves by writing "<file>.tmp" and renaming it over the
 * file (settings, memory cards, save states, stats...): none of those saves ever replaced the old file. */
#include <stdio.h>
int SwanStationPS5_rename(const char *from, const char *to);
#define rename(from, to) SwanStationPS5_rename(from, to)
#endif
#endif
