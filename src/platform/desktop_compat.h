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
#endif
#endif
