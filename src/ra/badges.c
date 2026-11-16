/*
 * SwanStationPS5 - achievement badges drawn while playing (challenge indicators).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "badges.h"

#include "../app.h"
#include "../net.h"
#include "stb_image.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 16

enum
{
    BADGE_NONE = 0,
    BADGE_FETCHING,
    BADGE_FILE, /* on disk, not a texture yet */
    BADGE_FAILED,
};

typedef struct
{
    unsigned id;
    char url[160];
    char path[SwanStationPS5_PATH_MAX];
    SDL_atomic_t state;
    PlatTexture *texture;
} Slot;

static Slot slots[SLOTS];
static int next_slot;

static int fetch(void *arg)
{
    Slot *s = arg;
    char dir[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "cache/badges");
    make_dirs(dir);
    const bool ok = path_exists(s->path) || (s->url[0] && net_download(s->url, s->path) == NET_OK);
    SDL_AtomicSet(&s->state, ok ? BADGE_FILE : BADGE_FAILED);
    return 0;
}

PlatTexture *ra_badge(unsigned id, const char *url)
{
    Slot *s = NULL;
    for (int i = 0; i < SLOTS && !s; ++i)
        if (slots[i].id == id && (slots[i].texture || SDL_AtomicGet(&slots[i].state) != BADGE_NONE))
            s = &slots[i];
    if (!s)
    {
        /* a free slot, else the next one in turn (its picture goes: only a few challenges are up at once) */
        s = &slots[next_slot];
        next_slot = (next_slot + 1) % SLOTS;
        for (int i = 0; i < SLOTS; ++i)
            if (!slots[i].texture && SDL_AtomicGet(&slots[i].state) == BADGE_NONE)
            {
                s = &slots[i];
                break;
            }
        if (SDL_AtomicGet(&s->state) == BADGE_FETCHING)
            return NULL; /* its download is still running: try again next frame */
        plat_texture_free(s->texture);
        s->texture = NULL;
        s->id = id;
        str_copy(s->url, sizeof(s->url), url ? url : "");
        char file[48];
        snprintf(file, sizeof(file), "cache/badges/%u_1.png", id);
        path_join(s->path, sizeof(s->path), app.paths.root, file);
        SDL_AtomicSet(&s->state, BADGE_FETCHING);
        SDL_Thread *t = SDL_CreateThread(fetch, "badge", s);
        if (t)
            SDL_DetachThread(t);
        else
            SDL_AtomicSet(&s->state, BADGE_FAILED);
        return NULL;
    }
    if (s->texture)
        return s->texture;
    if (SDL_AtomicGet(&s->state) == BADGE_FILE)
    {
        int w, h, comp;
        unsigned char *px = stbi_load(s->path, &w, &h, &comp, 4);
        if (px)
        {
            s->texture = plat_texture_create(px, w, h, true);
            stbi_image_free(px);
        }
        if (!s->texture)
            SDL_AtomicSet(&s->state, BADGE_FAILED);
    }
    return s->texture;
}

PlatTexture *ra_badge_cached(unsigned id)
{
    char file[48], path[SwanStationPS5_PATH_MAX];
    snprintf(file, sizeof(file), "cache/badges/%u_1.png", id);
    path_join(path, sizeof(path), app.paths.root, file);
    if (!path_exists(path))
        return NULL;
    int w, h, comp;
    unsigned char *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px)
        return NULL;
    PlatTexture *t = plat_texture_create(px, w, h, true);
    stbi_image_free(px);
    return t;
}

typedef struct
{
    RaBadgeJob *jobs;
    int count;
    int generation;
} Prefetch;

static SDL_atomic_t prefetch_generation;

static void prefetch_one(unsigned id, int state, const char *url)
{
    char file[48], path[SwanStationPS5_PATH_MAX];
    snprintf(file, sizeof(file), "cache/badges/%u_%d.png", id, state);
    path_join(path, sizeof(path), app.paths.root, file);
    if (url[0] && !path_exists(path))
        net_download(url, path);
}

static int prefetch_run(void *arg)
{
    Prefetch *p = arg;
    char dir[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "cache/badges");
    make_dirs(dir);
    for (int i = 0; i < p->count && SDL_AtomicGet(&prefetch_generation) == p->generation; ++i)
    {
        prefetch_one(p->jobs[i].id, 1, p->jobs[i].url_unlocked);
        if (!p->jobs[i].unlocked && SDL_AtomicGet(&prefetch_generation) == p->generation)
            prefetch_one(p->jobs[i].id, 0, p->jobs[i].url_locked);
    }
    free(p->jobs);
    free(p);
    return 0;
}

void ra_badges_prefetch_stop(void)
{
    SDL_AtomicAdd(&prefetch_generation, 1);
}

void ra_badges_prefetch(const RaBadgeJob *jobs, int count)
{
    ra_badges_prefetch_stop();
    if (count <= 0)
        return;
    Prefetch *p = malloc(sizeof(*p));
    RaBadgeJob *copy = malloc((size_t)count * sizeof(*copy));
    if (!p || !copy)
    {
        free(p);
        free(copy);
        return;
    }
    memcpy(copy, jobs, (size_t)count * sizeof(*copy));
    p->jobs = copy;
    p->count = count;
    p->generation = SDL_AtomicGet(&prefetch_generation);
    SDL_Thread *t = SDL_CreateThread(prefetch_run, "badges-prefetch", p);
    if (t)
        SDL_DetachThread(t);
    else
    {
        free(copy);
        free(p);
    }
}

void ra_badges_shutdown(void)
{
    ra_badges_prefetch_stop();
    for (int i = 0; i < SLOTS; ++i)
    {
        plat_texture_free(slots[i].texture);
        slots[i].texture = NULL;
    }
}
