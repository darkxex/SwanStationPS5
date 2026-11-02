/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "state_save.h"
#include "core/host.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char path[SwanStationPS5_PATH_MAX], directory[SwanStationPS5_PATH_MAX];
    void *data;
    size_t size;
    bool have_thumb;
    uint8_t thumb[THUMB_W * THUMB_H * 4];
} StateWrite;

static SDL_Thread *worker;
static SDL_mutex *lock;
static SDL_cond *wake;
static SDL_atomic_t busy;
static StateWrite *pending, *capturing;
static bool quit, saved;

static void free_job(StateWrite *job)
{
    free(job->data);
    free(job);
}

static bool write_job(StateWrite *job)
{
    char temp[SwanStationPS5_PATH_MAX + 16], thumb[SwanStationPS5_PATH_MAX + 8];
    make_dirs(job->directory);
    snprintf(temp, sizeof(temp), "%s.tmp", job->path);
    FILE *f = fopen(temp, "wb");
    bool ok = f && fwrite(job->data, 1, job->size, f) == job->size;
    if (f)
        ok = fclose(f) == 0 && ok;
    ok = ok && rename(temp, job->path) == 0;
    if (!ok)
    {
        remove(temp);
        return false;
    }
    snprintf(thumb, sizeof(thumb), "%s.thumb", job->path);
    snprintf(temp, sizeof(temp), "%s.tmp", thumb);
    bool thumb_ok = false;
    if (job->have_thumb)
    {
        for (size_t i = 3; i < sizeof(job->thumb); i += 4)
            job->thumb[i] = 255;
        const uint8_t header[8] = {'P', 'S', 'X', 'T', THUMB_W & 0xff, THUMB_W >> 8,
                                 THUMB_H & 0xff, THUMB_H >> 8};
        f = fopen(temp, "wb");
        thumb_ok = f && fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
                   fwrite(job->thumb, 1, sizeof(job->thumb), f) == sizeof(job->thumb);
        if (f)
            thumb_ok = fclose(f) == 0 && thumb_ok;
        thumb_ok = thumb_ok && rename(temp, thumb) == 0;
    }
    if (!thumb_ok)
    {
        remove(temp);
        remove(thumb);
    }
    return true;
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
        StateWrite *job = pending;
        pending = NULL;
        SDL_UnlockMutex(lock);
        bool ok = write_job(job);
        free_job(job);
        SDL_LockMutex(lock);
        saved = ok;
        SDL_AtomicSet(&busy, 0);
        SDL_CondBroadcast(wake);
    }
    SDL_UnlockMutex(lock);
    return 0;
}

void state_save_init(void)
{
    if (worker)
        return;
    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    if (lock && wake)
        worker = SDL_CreateThread(writer_main, "state-save", NULL);
    if (!worker)
    {
        SDL_DestroyCond(wake);
        SDL_DestroyMutex(lock);
        wake = NULL;
        lock = NULL;
    }
}

static void submit(StateWrite *job)
{
    if (!worker)
    {
        saved = write_job(job);
        free_job(job);
        return;
    }
    SDL_LockMutex(lock);
    SDL_AtomicSet(&busy, 1);
    pending = job;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
}

static void finish_capture(bool wait)
{
    if (!capturing)
        return;
    int result = host_capture_poll(capturing->thumb, THUMB_W, THUMB_H, wait);
    if (result == 0)
        return;
    StateWrite *job = capturing;
    capturing = NULL;
    job->have_thumb = result > 0;
    submit(job);
}

void state_save_poll(void)
{
    finish_capture(false);
}

void state_save_flush(void)
{
    finish_capture(true);
    if (!worker)
        return;
    SDL_LockMutex(lock);
    while (SDL_AtomicGet(&busy))
        SDL_CondWait(wake, lock);
    SDL_UnlockMutex(lock);
}

bool state_save_request(const char *path, const char *directory, bool background)
{
    if (background && (!worker || capturing || SDL_AtomicGet(&busy)))
        return false;
    if (!background)
        state_save_flush();
    size_t size = host_state_size();
    StateWrite *job = size ? calloc(1, sizeof(*job)) : NULL;
    if (!job)
        return false;
    job->data = malloc(size);
    job->size = size;
    str_copy(job->path, sizeof(job->path), path);
    str_copy(job->directory, sizeof(job->directory), directory);
    /* The core and its GPU state must be sampled between emulated frames. */
    bool ok = job->data && host_serialize(job->data, size);
    if (!ok)
    {
        free_job(job);
        return false;
    }
    int result = host_capture_begin(job->thumb, THUMB_W, THUMB_H);
    job->have_thumb = result > 0;
    if (result == 0)
        capturing = job;
    else
        submit(job);
    if (background)
        return true;
    state_save_flush();
    return saved;
}

void state_save_shutdown(void)
{
    state_save_flush();
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
    quit = saved = false;
}
