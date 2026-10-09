/*
 * SwanStationPS5 - logging and path helpers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "SwanStationPS5.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static FILE *log_file;

void SwanStationPS5_log_open(const char *path)
{
    if (log_file)
        fclose(log_file);
    log_file = fopen(path, "w");
}

void SwanStationPS5_log(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    size_t len = strlen(line);
    const char *nl = (len > 0 && line[len - 1] == '\n') ? "" : "\n";
    fprintf(stderr, "[SwanStationPS5] %s%s", line, nl);
    if (log_file)
    {
        fprintf(log_file, "%s%s", line, nl);
        fflush(log_file);
    }
}

void str_copy(char *dst, size_t size, const char *src)
{
    if (size == 0)
        return;
    size_t n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void path_join(char *dst, size_t size, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la > 0 && a[la - 1] == '/')
        snprintf(dst, size, "%s%s", a, b);
    else
        snprintf(dst, size, "%s/%s", a, b);
}

bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool make_dirs(const char *path)
{
    char tmp[SwanStationPS5_PATH_MAX];
    str_copy(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; ++p)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0777) == 0 || errno == EEXIST;
}

bool file_copy(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return false;
    FILE *out = fopen(to, "wb");
    bool ok = out != NULL;
    char buf[64 * 1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out && fclose(out) != 0)
        ok = false;
    if (!ok)
        remove(to);
    return ok;
}

const char *path_ext(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash))
        return "";
    return dot + 1;
}

int str_icmp(const char *a, const char *b)
{
    for (;; ++a, ++b)
    {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || ca == 0)
            return ca - cb;
    }
}

bool local_time(long long when, struct tm *out)
{
    time_t t = (time_t)when;
    struct tm *r = localtime(&t); /* main thread only */
    if (!r)
        return false;
    *out = *r;
    return true;
}
