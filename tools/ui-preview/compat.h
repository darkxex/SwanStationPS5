/*
 * SwanStationPS5 interface preview - what the Windows (mingw) C library lacks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_PREVIEW_COMPAT_H
#define SwanStationPS5_PREVIEW_COMPAT_H
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <time.h>
#define mkdir(path, mode) _mkdir(path)
static inline struct tm *SwanStationPS5_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : 0;
}
#define localtime_r SwanStationPS5_localtime_r
#endif
#endif
