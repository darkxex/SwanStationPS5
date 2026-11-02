/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DARWIN_C_SOURCE
#include "state_save.h"
#include "core/host.h"

#include <SDL2/SDL.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static SDL_threadID main_thread;
static SDL_mutex *gate;
static SDL_cond *changed;
static SDL_atomic_t snapshots;
static int writes, failure;
static bool blocked, allow_main_io, fail_thread;
static uint8_t live_value, capture_value;
static int capture_mode;
static bool capture_ready, serialize_ok = true;

size_t host_state_size(void)
{
    assert(SDL_ThreadID() == main_thread);
    return 64;
}

bool host_serialize(void *buffer, size_t size)
{
    assert(SDL_ThreadID() == main_thread);
    SDL_AtomicAdd(&snapshots, 1);
    memset(buffer, live_value, size);
    return serialize_ok;
}

int host_capture_begin(uint8_t *rgba, int w, int h)
{
    assert(SDL_ThreadID() == main_thread);
    capture_value = live_value;
    if (capture_mode > 0)
        memset(rgba, capture_value, (size_t)w * h * 4);
    return capture_mode;
}

int host_capture_poll(uint8_t *rgba, int w, int h, bool wait)
{
    assert(SDL_ThreadID() == main_thread);
    if (!wait && !capture_ready)
        return 0;
    if (capture_mode < 0)
        return -1;
    memset(rgba, capture_value, (size_t)w * h * 4);
    return 1;
}

SDL_Thread *state_test_create_thread(SDL_ThreadFunction fn, const char *name, void *arg)
{
    return fail_thread ? NULL : SDL_CreateThread(fn, name, arg);
}

bool state_test_make_dirs(const char *path)
{
    assert(allow_main_io || SDL_ThreadID() != main_thread);
    return make_dirs(path);
}

FILE *state_test_fopen(const char *path, const char *mode)
{
    assert(allow_main_io || SDL_ThreadID() != main_thread);
    SDL_LockMutex(gate);
    ++writes;
    SDL_CondBroadcast(changed);
    while (blocked)
        SDL_CondWait(changed, gate);
    bool fail = failure == 1 || (failure == 2 && strstr(path, ".thumb.tmp"));
    SDL_UnlockMutex(gate);
    if (fail)
    {
        errno = EACCES;
        return NULL;
    }
    return fopen(path, mode);
}

static int block_writer(void)
{
    SDL_LockMutex(gate);
    blocked = true;
    int target = writes + 1;
    SDL_UnlockMutex(gate);
    return target;
}

static void wait_for_write(int target)
{
    SDL_LockMutex(gate);
    while (writes < target)
        SDL_CondWait(changed, gate);
    SDL_UnlockMutex(gate);
}

static int write_count(void)
{
    SDL_LockMutex(gate);
    int n = writes;
    SDL_UnlockMutex(gate);
    return n;
}

static int release_writer(void *arg)
{
    const int before = *(const int *)arg;
    SDL_Delay(30);
    /* A synchronous save must drain the old snapshot before capturing again. */
    assert(SDL_AtomicGet(&snapshots) == before);
    SDL_LockMutex(gate);
    blocked = false;
    SDL_CondBroadcast(changed);
    SDL_UnlockMutex(gate);
    return 0;
}

static void verify(const char *path, uint8_t value, bool thumb)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    for (int i = 0; i < 64; ++i)
        assert(fgetc(f) == value);
    assert(fgetc(f) == EOF);
    assert(fclose(f) == 0);
    char name[SwanStationPS5_PATH_MAX + 16];
    snprintf(name, sizeof(name), "%s.thumb", path);
    f = fopen(name, "rb");
    if (thumb)
    {
        assert(f);
        uint8_t header[8];
        assert(fread(header, 1, 8, f) == 8 && !memcmp(header, "PSXT", 4));
        assert((header[4] | header[5] << 8) == THUMB_W);
        assert((header[6] | header[7] << 8) == THUMB_H);
        for (int i = 0; i < THUMB_W * THUMB_H * 4; ++i)
            assert(fgetc(f) == (i % 4 == 3 ? 255 : value));
        assert(fgetc(f) == EOF);
        assert(fclose(f) == 0);
    }
    else
        assert(!f);
    snprintf(name, sizeof(name), "%s.tmp", path);
    assert(access(name, F_OK) != 0);
    snprintf(name, sizeof(name), "%s.thumb.tmp", path);
    assert(access(name, F_OK) != 0);
}

int main(void)
{
    alarm(20);
    assert(SDL_Init(0) == 0);
    main_thread = SDL_ThreadID();
    gate = SDL_CreateMutex();
    changed = SDL_CreateCond();
    assert(gate && changed);
    char root[] = "build/tests/state-save-XXXXXX";
    assert(mkdtemp(root));
    char directory[SwanStationPS5_PATH_MAX], path[SwanStationPS5_PATH_MAX];
    path_join(directory, sizeof(directory), root, "states");
    path_join(path, sizeof(path), directory, "game.resume");
    state_save_init();

    live_value = 17;
    assert(state_save_request(path, directory, true));
    live_value = 99;
    int snapshot_count = SDL_AtomicGet(&snapshots);
    assert(!state_save_request(path, directory, true));
    assert(SDL_AtomicGet(&snapshots) == snapshot_count);
    state_save_poll();
    assert(write_count() == 0 && access(directory, F_OK) != 0);
    int target = block_writer();
    capture_ready = true;
    state_save_poll();
    wait_for_write(target);
    assert(!state_save_request(path, directory, true));
    assert(SDL_AtomicGet(&snapshots) == snapshot_count);
    state_save_poll();
    SDL_Thread *release = SDL_CreateThread(release_writer, "release", &snapshot_count);
    assert(release);
    state_save_flush();
    SDL_WaitThread(release, NULL);
    verify(path, 17, true);

    capture_mode = 1;
    live_value = 23;
    target = block_writer();
    assert(state_save_request(path, directory, true));
    wait_for_write(target);
    live_value = 42;
    snapshot_count = SDL_AtomicGet(&snapshots);
    release = SDL_CreateThread(release_writer, "release", &snapshot_count);
    assert(release);
    assert(state_save_request(path, directory, false));
    SDL_WaitThread(release, NULL);
    verify(path, 42, true);

    serialize_ok = false;
    int before = write_count();
    assert(!state_save_request(path, directory, true));
    state_save_flush();
    assert(write_count() == before);
    verify(path, 42, true);
    serialize_ok = true;

    SDL_LockMutex(gate);
    failure = 1;
    SDL_UnlockMutex(gate);
    assert(!state_save_request(path, directory, false));
    verify(path, 42, true);
    SDL_LockMutex(gate);
    failure = 2;
    SDL_UnlockMutex(gate);
    live_value = 57;
    assert(state_save_request(path, directory, false));
    verify(path, 57, false);
    SDL_LockMutex(gate);
    failure = 0;
    SDL_UnlockMutex(gate);

    capture_mode = 0;
    capture_ready = false;
    live_value = 63;
    char old_path[SwanStationPS5_PATH_MAX], old_directory[SwanStationPS5_PATH_MAX];
    str_copy(old_path, sizeof(old_path), path);
    str_copy(old_directory, sizeof(old_directory), directory);
    assert(state_save_request(path, directory, true));
    path_join(directory, sizeof(directory), root, "other-profile");
    path_join(path, sizeof(path), directory, "other.resume");
    live_value = 81;
    state_save_shutdown(); /* Also finishes a GPU readback not yet polled. */
    verify(old_path, 63, true);
    assert(access(path, F_OK) != 0);

    state_save_init();
    capture_mode = -1;
    assert(state_save_request(path, directory, true));
    state_save_shutdown();
    verify(path, 81, false);

    state_save_init();
    capture_mode = 0;
    assert(state_save_request(old_path, old_directory, true));
    capture_mode = -1; /* A device reset loses the pending thumbnail, not the state. */
    state_save_flush();
    verify(old_path, 81, false);
    state_save_shutdown();

    fail_thread = true;
    state_save_init();
    before = write_count();
    snapshot_count = SDL_AtomicGet(&snapshots);
    assert(!state_save_request(path, directory, true));
    assert(write_count() == before && SDL_AtomicGet(&snapshots) == snapshot_count);
    allow_main_io = true;
    live_value = 93;
    assert(state_save_request(path, directory, false));
    verify(path, 93, false);
    state_save_shutdown();

    assert(remove(old_path) == 0 && rmdir(old_directory) == 0);
    assert(remove(path) == 0 && rmdir(directory) == 0 && rmdir(root) == 0);
    SDL_DestroyCond(changed);
    SDL_DestroyMutex(gate);
    SDL_Quit();
    puts("Async state/thumbnail snapshot, blocked-write, ordering, failure and shutdown checks passed.");
    return 0;
}
