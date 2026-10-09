/*
 * SwanStationPS5 - fast multi-threaded scaling of the game picture into the screen.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On PS5 everything is drawn by the CPU. SDL's generic software stretch
 * took ~20 ms per frame for the game picture; this does the same job with
 * precomputed 8-bit fixed-point bilinear weights, two channels per multiply,
 * the XRGB -> ABGR swap and the menu dimming in one pass, across threads.
 */
#include "blit.h"

#include <SDL2/SDL.h>
#include <string.h>

#define WORKERS 3 /* plus the calling thread: 4 slices */
#define MAX_DST 4096

static SDL_Thread *threads[WORKERS];
static SDL_sem *start_sem[WORKERS], *done_sem;
static const BlitJob *current;
static int slices = 1;
static bool ready;

/* per-column source index and weight, shared by all rows */
static int col_x0[MAX_DST], col_x1[MAX_DST];
static uint32_t col_f[MAX_DST];

static inline uint32_t lerp2(uint32_t a, uint32_t b, uint32_t f) /* f in 0..256 */
{
    uint32_t inv = 256 - f;
    uint32_t rb = ((a & 0x00ff00ffu) * inv + (b & 0x00ff00ffu) * f) >> 8;
    uint32_t g = (((a >> 8) & 0x00ff00ffu) * inv + ((b >> 8) & 0x00ff00ffu) * f);
    return (rb & 0x00ff00ffu) | (g & 0xff00ff00u);
}

/* XRGB (0x..RRGGBB) -> ABGR (0xffBBGGRR), with optional dimming */
static inline uint32_t to_canvas(uint32_t c, uint32_t dim)
{
    if (dim < 255)
    {
        uint32_t rb = ((c & 0x00ff00ffu) * dim >> 8) & 0x00ff00ffu;
        uint32_t g = ((c & 0x0000ff00u) * dim >> 8) & 0x0000ff00u;
        c = rb | g;
    }
    return 0xff000000u | ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
}

static void run_slice(const BlitJob *j, int slice)
{
    const int y_begin = j->dst_h * slice / slices, y_end = j->dst_h * (slice + 1) / slices;
    for (int y = y_begin; y < y_end; ++y)
    {
        uint32_t dim = j->dim;
        if (j->scanlines && j->lines > 0)
        {
            /* where this row falls inside its game line: bright in the middle,
             * darker towards the gap */
            int64_t pos = ((int64_t)y * 2 + 1) * j->lines * 128 / j->dst_h; /* 256 per line */
            int phase = (int)(pos & 255) - 128;                               /* -128..127 */
            uint32_t edge = (uint32_t)(phase * phase) >> 6;                    /* 0..256 */
            dim = dim * (256 - ((edge * j->scanlines) >> 8)) >> 8;
        }
        uint32_t *out = j->dst + (size_t)(j->dst_y + y) * j->dst_pitch + j->dst_x;
        /* source row position in 16.16, sampling pixel centres */
        int64_t sy = (((int64_t)y * 2 + 1) * j->src_h << 16) / (2 * j->dst_h) - 32768;
        if (sy < 0)
            sy = 0;
        int y0 = (int)(sy >> 16), y1 = y0 + 1 < j->src_h ? y0 + 1 : y0;
        uint32_t fy = j->smooth ? (uint32_t)((sy >> 8) & 0xff) : 0;
        if (!j->smooth && ((sy >> 15) & 1) && y1 != y0)
            y0 = y1; /* nearest: round */
        const uint32_t *r0 = j->src + (size_t)y0 * j->src_pitch;
        const uint32_t *r1 = j->src + (size_t)y1 * j->src_pitch;
        if (j->smooth)
            for (int x = 0; x < j->dst_w; ++x)
            {
                uint32_t top = lerp2(r0[col_x0[x]], r0[col_x1[x]], col_f[x]);
                uint32_t bottom = lerp2(r1[col_x0[x]], r1[col_x1[x]], col_f[x]);
                out[x] = to_canvas(lerp2(top, bottom, fy), dim);
            }
        else
            for (int x = 0; x < j->dst_w; ++x)
                out[x] = to_canvas(r0[col_x0[x]], dim);
    }
}

/* A generic row job, or NULL for the blit in `current`. */
static void (*job_fn)(void *ctx, int row_begin, int row_end);
static void *job_ctx;
static int job_rows;

static void run_any(int slice)
{
    if (job_fn)
        job_fn(job_ctx, job_rows * slice / slices, job_rows * (slice + 1) / slices);
    else
        run_slice(current, slice);
}

static int worker(void *arg)
{
    int index = (int)(intptr_t)arg;
    for (;;)
    {
        SDL_SemWait(start_sem[index]);
        run_any(index + 1);
        SDL_SemPost(done_sem);
    }
    return 0;
}

static void dispatch(void)
{
    for (int i = 0; i < slices - 1; ++i)
        SDL_SemPost(start_sem[i]);
    run_any(0);
    for (int i = 0; i < slices - 1; ++i)
        SDL_SemWait(done_sem);
}

void blit_parallel(void (*fn)(void *ctx, int row_begin, int row_end), void *ctx, int rows)
{
    if (!ready)
        blit_init();
    job_fn = fn;
    job_ctx = ctx;
    job_rows = rows;
    dispatch();
    job_fn = NULL;
}

void blit_init(void)
{
    if (ready)
        return;
    done_sem = SDL_CreateSemaphore(0);
    slices = 1;
    for (int i = 0; i < WORKERS && done_sem; ++i)
    {
        start_sem[i] = SDL_CreateSemaphore(0);
        threads[i] = start_sem[i] ? SDL_CreateThread(worker, "blit", (void *)(intptr_t)i) : NULL;
        if (!threads[i])
            break;
        ++slices;
    }
    ready = true;
}

void blit_scaled(const BlitJob *j)
{
    if (j->dst_w <= 0 || j->dst_h <= 0 || j->src_w <= 0 || j->src_h <= 0 || j->dst_w > MAX_DST)
        return;
    if (!ready)
        blit_init();
    for (int x = 0; x < j->dst_w; ++x)
    {
        int64_t sx = (((int64_t)x * 2 + 1) * j->src_w << 16) / (2 * j->dst_w) - 32768;
        if (sx < 0)
            sx = 0;
        int x0 = (int)(sx >> 16);
        col_x1[x] = x0 + 1 < j->src_w ? x0 + 1 : x0;
        if (j->smooth)
        {
            col_x0[x] = x0;
            col_f[x] = (uint32_t)((sx >> 8) & 0xff);
        }
        else
            col_x0[x] = ((sx >> 15) & 1) ? col_x1[x] : x0; /* nearest: round */
    }
    current = j;
    job_fn = NULL;
    dispatch();
}
