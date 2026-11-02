/*
 * SwanStationPS5 - per-game records: play time, last played, favorite, achievements.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * stats.txt: id<TAB>seconds<TAB>last played (unix)<TAB>favorite<TAB>unlocked<TAB>total
 */
#include "stats.h"

#include "i18n.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GameStats *records;
static int count, capacity;
static char path[SwanStationPS5_PATH_MAX];

typedef struct
{
    char path[SwanStationPS5_PATH_MAX];
    int count;
    GameStats records[];
} StatsWrite;

static SDL_Thread *worker;
static SDL_mutex *lock;
static SDL_cond *wake;
static StatsWrite *pending;
static bool writing, quit;

static void write_records(const char *dest, const GameStats *items, int n)
{
    if (!dest[0])
        return;
    char temp[SwanStationPS5_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", dest);
    FILE *f = fopen(temp, "w");
    if (!f)
        return;
    fprintf(f, "# SwanStationPS5 play records: id, seconds played, last played, favorite, achievements\n");
    for (int i = 0; i < n; ++i)
    {
        const GameStats *g = &items[i];
        if (!g->seconds && !g->last_played && !g->favorite && g->ach_unlocked < 0)
            continue;
        fprintf(f, "%s\t%u\t%lld\t%d\t%d\t%d\n", g->id, g->seconds, (long long)g->last_played,
                g->favorite ? 1 : 0, g->ach_unlocked, g->ach_total);
    }
    bool ok = !ferror(f);
    if (fclose(f) == 0 && ok)
        rename(temp, dest);
}

static int writer_main(void *unused)
{
    (void)unused;
    SDL_LockMutex(lock);
    for (;;)
    {
        while (!pending && !quit)
            SDL_CondWait(wake, lock);
        if (!pending && quit)
            break;
        StatsWrite *job = pending;
        pending = NULL;
        writing = true;
        SDL_UnlockMutex(lock);
        write_records(job->path, job->records, job->count);
        free(job);
        SDL_LockMutex(lock);
        writing = false;
        SDL_CondBroadcast(wake);
    }
    SDL_UnlockMutex(lock);
    return 0;
}

static void writer_start(void)
{
    if (worker)
        return;
    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    if (lock && wake)
        worker = SDL_CreateThread(writer_main, "stats", NULL);
    if (!worker)
    {
        SDL_DestroyCond(wake);
        SDL_DestroyMutex(lock);
        wake = NULL;
        lock = NULL;
    }
}

static void writer_flush(void)
{
    if (!worker)
        return;
    SDL_LockMutex(lock);
    while (pending || writing)
        SDL_CondWait(wake, lock);
    SDL_UnlockMutex(lock);
}

GameStats *stats_get(const char *id)
{
    if (!id || !id[0])
        return NULL;
    for (int i = 0; i < count; ++i)
        if (strcmp(records[i].id, id) == 0)
            return &records[i];
    if (count == capacity)
    {
        int cap = capacity ? capacity * 2 : 64;
        GameStats *grown = realloc(records, sizeof(GameStats) * (size_t)cap);
        if (!grown)
            return NULL;
        records = grown;
        capacity = cap;
    }
    GameStats *g = &records[count++];
    memset(g, 0, sizeof(*g));
    str_copy(g->id, sizeof(g->id), id);
    g->ach_unlocked = -1;
    return g;
}

void stats_load(const char *root)
{
    writer_flush();
    writer_start();
    path_join(path, sizeof(path), root, "stats.txt");
    count = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#')
            continue;
        char *fields[6] = {0};
        int n = 0;
        for (char *p = line; p && n < 7;)
        {
            fields[n++] = p;
            p = strchr(p, '\t');
            if (p)
                *p++ = '\0';
        }
        if (n < 4)
            continue;
        fields[n - 1][strcspn(fields[n - 1], "\r\n")] = '\0';
        GameStats *g = stats_get(fields[0]);
        if (!g)
            break;
        g->seconds = (uint32_t)strtoul(fields[1], NULL, 10);
        g->last_played = strtoll(fields[2], NULL, 10);
        g->favorite = atoi(fields[3]) != 0;
        if (n >= 6)
        {
            g->ach_unlocked = atoi(fields[4]);
            g->ach_total = atoi(fields[5]);
        }
    }
    fclose(f);
}

void stats_save(void)
{
    writer_flush();
    write_records(path, records, count);
}

void stats_save_async(void)
{
    if (!worker || !path[0])
        return;
    size_t bytes = sizeof(GameStats) * (size_t)count;
    StatsWrite *job = malloc(sizeof(*job) + bytes);
    if (!job)
        return;
    str_copy(job->path, sizeof(job->path), path);
    job->count = count;
    if (bytes)
        memcpy(job->records, records, bytes);
    SDL_LockMutex(lock);
    StatsWrite *old = pending;
    pending = job;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
    free(old);
}

void stats_shutdown(void)
{
    stats_save();
    if (worker)
    {
        SDL_LockMutex(lock);
        quit = true;
        SDL_CondSignal(wake);
        SDL_UnlockMutex(lock);
        SDL_WaitThread(worker, NULL);
        worker = NULL;
    }
    SDL_DestroyCond(wake);
    SDL_DestroyMutex(lock);
    wake = NULL;
    lock = NULL;
    quit = false;
    free(records);
    records = NULL;
    count = capacity = 0;
    path[0] = '\0';
}

void stats_format_time(uint32_t seconds, char *out, size_t size)
{
    uint32_t minutes = seconds / 60, hours = minutes / 60;
    if (seconds == 0)
        out[0] = '\0';
    else if (hours)
        snprintf(out, size, tr("%u h %02u min"), hours, minutes % 60);
    else
        snprintf(out, size, tr("%u min"), minutes ? minutes : 1);
}

void stats_format_when(int64_t when, char *out, size_t size)
{
    if (when <= 0)
    {
        out[0] = '\0';
        return;
    }
    time_t now = time(NULL);
    struct tm a, b;
    time_t then = (time_t)when;
    local_time(now, &a);
    local_time(then, &b);
    long days = (long)((now - a.tm_hour * 3600 - a.tm_min * 60 - a.tm_sec) - then);
    days = days <= 0 ? 0 : days / 86400 + 1;
    if (days == 0)
        str_copy(out, size, tr("Today"));
    else if (days == 1)
        str_copy(out, size, tr("Yesterday"));
    else if (days < 7)
        snprintf(out, size, tr("%ld days ago"), days);
    else if (days < 31)
        days < 14 ? str_copy(out, size, tr("Last week"))
                  : (void)snprintf(out, size, tr("%ld weeks ago"), days / 7);
    else
        snprintf(out, size, "%d-%02d-%02d", b.tm_year + 1900, b.tm_mon + 1, b.tm_mday);
}
