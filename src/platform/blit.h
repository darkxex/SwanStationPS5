/*
 * SwanStationPS5 - fast multi-threaded scaling of the game picture into the screen.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_BLIT_H
#define SwanStationPS5_BLIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const uint32_t *src; /* XRGB8888 (the core's 32-bit output) */
    int src_w, src_h;
    size_t src_pitch;    /* in pixels */
    uint32_t *dst;       /* ABGR8888 canvas (R,G,B,A bytes) */
    size_t dst_pitch;    /* in pixels */
    int dst_x, dst_y, dst_w, dst_h;
    bool smooth;         /* bilinear, else nearest */
    uint8_t dim;         /* 255 = full brightness */
    uint8_t scanlines;   /* CRT look: how dark the gaps between lines get (0 = off) */
    int lines;           /* the game's own line count, for the scanlines */
} BlitJob;

void blit_init(void);
/* Runs fn(ctx, row_begin, row_end) over [0, rows) split across the threads. */
void blit_parallel(void (*fn)(void *ctx, int row_begin, int row_end), void *ctx, int rows);
/* Scales job->src into the rectangle of job->dst, split across worker threads. */
void blit_scaled(const BlitJob *job);

#endif
