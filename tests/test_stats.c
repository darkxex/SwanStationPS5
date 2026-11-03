/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DARWIN_C_SOURCE
#include "stats.h"
#include "i18n.h"

#include <SDL2/SDL.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static SDL_mutex *gate;
static SDL_cond *changed;
static bool blocked, fail_write;
static int writes;

const char *tr(const char *english)
{
    return english;
}

/* Interpose only stats.c's opens; the writer still performs real file I/O. */
FILE *stats_test_fopen(const char *path, const char *mode)
{
    if (!strcmp(mode, "w"))
    {
        SDL_LockMutex(gate);
        ++writes;
        SDL_CondBroadcast(changed);
        while (blocked)
            SDL_CondWait(changed, gate);
        bool fail = fail_write;
        SDL_UnlockMutex(gate);
        if (fail)
        {
            errno = EACCES;
            return NULL;
        }
    }
    return fopen(path, mode);
}

static int block_writer(void)
{
    SDL_LockMutex(gate);
    blocked = true;
    int next = writes + 1;
    SDL_UnlockMutex(gate);
    return next;
}

static void wait_for_write(int target)
{
    SDL_LockMutex(gate);
    while (writes < target)
        SDL_CondWait(changed, gate);
    SDL_UnlockMutex(gate);
}

static void release_writer(void)
{
    SDL_LockMutex(gate);
    blocked = false;
    SDL_CondBroadcast(changed);
    SDL_UnlockMutex(gate);
}

typedef enum { SAVE, LOAD, SHUTDOWN } Operation;

typedef struct
{
    Operation op;
    const char *root;
    bool started, done;
} Boundary;

static int run_boundary(void *arg)
{
    Boundary *b = arg;
    SDL_LockMutex(gate);
    b->started = true;
    SDL_CondBroadcast(changed);
    SDL_UnlockMutex(gate);
    switch (b->op)
    {
    case SAVE: stats_save(); break;
    case LOAD: stats_load(b->root); break;
    case SHUTDOWN: stats_shutdown(); break;
    }
    SDL_LockMutex(gate);
    b->done = true;
    SDL_CondBroadcast(changed);
    SDL_UnlockMutex(gate);
    return 0;
}

/* Hand the API to one caller while the test controls the blocked disk write. */
static void finish_while_blocked(Operation op, const char *root)
{
    Boundary b = {.op = op, .root = root};
    SDL_Thread *thread = SDL_CreateThread(run_boundary, "stats-test", &b);
    assert(thread);
    SDL_LockMutex(gate);
    while (!b.started)
        SDL_CondWait(changed, gate);
    Uint64 until = SDL_GetTicks64() + 30;
    while (SDL_GetTicks64() < until)
    {
        assert(!b.done);
        assert(SDL_CondWaitTimeout(changed, gate, 5) >= 0);
    }
    assert(!b.done);
    SDL_UnlockMutex(gate);
    release_writer();
    SDL_WaitThread(thread, NULL);
    assert(b.done);
}

static GameStats disk_record(const char *root, const char *id)
{
    char file[SwanStationPS5_PATH_MAX], line[256];
    path_join(file, sizeof(file), root, "stats.txt");
    FILE *f = fopen(file, "r");
    assert(f);
    while (fgets(line, sizeof(line), f))
    {
        GameStats g = {0};
        long long last;
        int favorite;
        if (sscanf(line, "%63s\t%u\t%lld\t%d\t%d\t%d", g.id, &g.seconds, &last,
                   &favorite, &g.ach_unlocked, &g.ach_total) != 6 || strcmp(g.id, id))
            continue;
        g.last_played = last;
        g.favorite = favorite != 0;
        assert(fclose(f) == 0);
        return g;
    }
    assert(!"missing saved record");
    abort();
}

static void remove_stats(const char *root)
{
    char file[SwanStationPS5_PATH_MAX];
    path_join(file, sizeof(file), root, "stats.txt");
    assert(remove(file) == 0);
    assert(rmdir(root) == 0);
}

int main(void)
{
    alarm(20);
    assert(SDL_Init(0) == 0);
    gate = SDL_CreateMutex();
    changed = SDL_CreateCond();
    assert(gate && changed);
    char root[] = "build/tests/stats-XXXXXX";
    assert(mkdtemp(root));
    char other[SwanStationPS5_PATH_MAX];
    path_join(other, sizeof(other), root, "other");
    assert(mkdir(other, 0700) == 0);

    stats_save_async();
    assert(writes == 0);
    stats_load(other);
    stats_get("other-game")->seconds = 700;
    stats_save();

    stats_load(root);
    stats_save_async(); /* An empty snapshot must also be valid. */
    stats_load(root);
    GameStats *g = stats_get("game");
    g->seconds = 10;
    g->last_played = 12345;
    g->favorite = true;
    g->ach_unlocked = 3;
    g->ach_total = 12;
    int target = block_writer();
    stats_save_async();
    wait_for_write(target);
    g->seconds = 99;
    g->favorite = false;
    for (int i = 0; i < 200; ++i)
    {
        char id[64];
        snprintf(id, sizeof(id), "extra-%d", i);
        stats_get(id)->seconds = 1000;
    }
    release_writer();
    stats_load(root);
    g = stats_get("game");
    assert(g->seconds == 10 && g->last_played == 12345 && g->favorite);
    assert(g->ach_unlocked == 3 && g->ach_total == 12);
    assert(stats_get("extra-199")->seconds == 0);

    target = block_writer();
    stats_get("game")->seconds = 20;
    stats_save_async();
    wait_for_write(target);
    for (unsigned seconds = 21; seconds <= 40; ++seconds)
    {
        stats_get("game")->seconds = seconds;
        stats_save_async();
    }
    stats_get("game")->seconds = 50;
    finish_while_blocked(SAVE, NULL);
    assert(writes == target + 2); /* Active, latest pending, then synchronous. */
    assert(disk_record(root, "game").seconds == 50);

    target = block_writer();
    stats_get("game")->seconds = 60;
    stats_save_async();
    wait_for_write(target);
    stats_get("game")->seconds = 70;
    stats_save_async();
    finish_while_blocked(LOAD, other);
    assert(disk_record(root, "game").seconds == 70);
    assert(stats_get("other-game")->seconds == 700);
    assert(stats_get("game")->seconds == 0);
    stats_get("other-game")->seconds = 701;
    stats_save_async();
    stats_load(root);
    assert(disk_record(other, "other-game").seconds == 701);
    assert(stats_get("game")->seconds == 70);

    SDL_LockMutex(gate);
    fail_write = true;
    SDL_UnlockMutex(gate);
    stats_get("game")->seconds = 80;
    stats_save_async();
    stats_load(root);
    assert(stats_get("game")->seconds == 70);
    SDL_LockMutex(gate);
    fail_write = false;
    SDL_UnlockMutex(gate);

    target = block_writer();
    stats_get("game")->seconds = 90;
    stats_save_async();
    wait_for_write(target);
    stats_get("game")->seconds = 95;
    stats_save_async();
    stats_get("game")->seconds = 100;
    finish_while_blocked(SHUTDOWN, NULL);
    assert(disk_record(root, "game").seconds == 100);
    stats_save_async();
    stats_shutdown();
    stats_load(root);
    stats_get("game")->seconds = 101;
    stats_save_async();
    stats_shutdown();
    assert(disk_record(root, "game").seconds == 101);

    remove_stats(other);
    remove_stats(root);
    SDL_DestroyCond(changed);
    SDL_DestroyMutex(gate);
    SDL_Quit();
    puts("Statistics snapshot, ordering, profile and shutdown checks passed.");
    return 0;
}
