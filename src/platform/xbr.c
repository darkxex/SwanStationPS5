/*
 * SwanStationPS5 - xBR 2x (level 2), the edge-directed pixel-art scaler by Hyllian.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * For each source pixel E the 5x5 neighbourhood decides, per output corner,
 * whether an edge crosses that corner and how steep it is, then blends the
 * corner towards the colour across the edge. Stair-steps become diagonals
 * and curves round off, while flat areas and straight edges stay sharp.
 *
 *          A1 B1 C1
 *       A0  A  B  C C4
 *       D0  D  E  F F4      E -> E0 E1
 *       G0  G  H  I I4           E2 E3
 *          G5 H5 I5
 *
 * Speed: every source pixel is converted to Y/U/V once (a prepass), so the
 * ~50 colour distances per pixel are three subtractions each instead of
 * multiplications and divisions. Rows run in parallel via blit_parallel().
 */
#include "xbr.h"

#include "blit.h"

#include <stdlib.h>

typedef struct
{
    int16_t y, u, v, pad;
} Yuv;

static Yuv *yuv;
static size_t yuv_size;

typedef struct
{
    const uint32_t *src;
    int w, h;
    size_t pitch;
    uint32_t *dst;
} Ctx;

static void convert_rows(void *opaque, int y_begin, int y_end)
{
    const Ctx *c = opaque;
    for (int y = y_begin; y < y_end; ++y)
        for (int x = 0; x < c->w; ++x)
        {
            uint32_t p = c->src[(size_t)y * c->pitch + x];
            int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            Yuv *o = &yuv[(size_t)y * c->w + x];
            o->y = (int16_t)((r * 299 + g * 587 + b * 114) / 1000);
            o->u = (int16_t)((r * -169 + g * -331 + b * 500) / 1000);
            o->v = (int16_t)((r * 500 + g * -419 + b * -81) / 1000);
        }
}

static inline int dist(const Yuv *a, const Yuv *b)
{
    return 48 * abs(a->y - b->y) + 7 * abs(a->u - b->u) + 6 * abs(a->v - b->v);
}

/* a + (b - a) * w / 4, per channel */
static inline uint32_t mix4(uint32_t a, uint32_t b, int w)
{
    uint32_t rb = ((a & 0xff00ffu) * (4 - w) + (b & 0xff00ffu) * w) >> 2;
    uint32_t g = ((a & 0x00ff00u) * (4 - w) + (b & 0x00ff00u) * w) >> 2;
    return (rb & 0xff00ffu) | (g & 0x00ff00u);
}

#define SAME 155 /* "equal enough": tolerates PS1 dithering noise */

/* One corner, written for the bottom-right case (E3 between F, H, I); the
 * other corners pass the neighbourhood rotated. Each argument is the colour
 * and its YUV. n3 is the corner itself, n1/n2 its neighbours on the F/H side. */
typedef struct
{
    uint32_t c;
    const Yuv *q;
} Px;

static inline void corner(Px e, Px f, Px h, Px i, Px c, Px g, Px f4, Px h5, Px i4, Px i5, Px b,
                          Px d, uint32_t *n3, uint32_t *n1, uint32_t *n2)
{
    int ef = dist(e.q, f.q), eh = dist(e.q, h.q);
    if (ef < SAME || eh < SAME)
        return;
    int w1 = dist(e.q, c.q) + dist(e.q, g.q) + dist(i.q, f4.q) + dist(i.q, h5.q) + 4 * dist(h.q, f.q);
    int w2 = dist(h.q, d.q) + dist(h.q, i5.q) + dist(f.q, i4.q) + dist(f.q, b.q) + 4 * dist(e.q, i.q);
    if (w1 >= w2)
        return;
    uint32_t px = ef <= eh ? f.c : h.c;
    int ke = dist(f.q, g.q), ki = dist(h.q, c.q);
    int shallow = 2 * ke <= ki && dist(e.q, g.q) >= SAME && dist(d.q, g.q) >= SAME;
    int steep = 2 * ki <= ke && dist(e.q, c.q) >= SAME && dist(b.q, c.q) >= SAME;
    *n3 = mix4(*n3, px, shallow || steep ? 3 : 2);
    if (shallow)
        *n2 = mix4(*n2, px, 1);
    if (steep)
        *n1 = mix4(*n1, px, 1);
}

static void scale_rows(void *opaque, int y_begin, int y_end)
{
    const Ctx *c = opaque;
    const int w = c->w, h = c->h;
    const size_t dpitch = (size_t)w * 2;
    for (int y = y_begin; y < y_end; ++y)
    {
        int ys[5];
        for (int k = 0; k < 5; ++k)
        {
            int yy = y + k - 2;
            ys[k] = yy < 0 ? 0 : yy >= h ? h - 1 : yy;
        }
        uint32_t *o0 = c->dst + (size_t)y * 2 * dpitch, *o1 = o0 + dpitch;
        for (int x = 0; x < w; ++x)
        {
            int xs[5];
            for (int k = 0; k < 5; ++k)
            {
                int xx = x + k - 2;
                xs[k] = xx < 0 ? 0 : xx >= w ? w - 1 : xx;
            }
#define P(dx, dy)                                                                                 \
    ((Px){c->src[(size_t)ys[(dy) + 2] * c->pitch + xs[(dx) + 2]],                                 \
          &yuv[(size_t)ys[(dy) + 2] * w + xs[(dx) + 2]]})
            /* Flat areas (most of a frame): every corner rule needs E to differ
             * from both of its side neighbours, so a pixel matching all four
             * can be copied straight away. */
            const Yuv *qe = &yuv[(size_t)ys[2] * w + xs[2]];
            const uint32_t ce = c->src[(size_t)ys[2] * c->pitch + xs[2]];
            if ((dist(qe, &yuv[(size_t)ys[2] * w + xs[3]]) < SAME ||
                 dist(qe, &yuv[(size_t)ys[3] * w + xs[2]]) < SAME) &&
                (dist(qe, &yuv[(size_t)ys[2] * w + xs[1]]) < SAME ||
                 dist(qe, &yuv[(size_t)ys[1] * w + xs[2]]) < SAME) &&
                (dist(qe, &yuv[(size_t)ys[2] * w + xs[1]]) < SAME ||
                 dist(qe, &yuv[(size_t)ys[3] * w + xs[2]]) < SAME) &&
                (dist(qe, &yuv[(size_t)ys[1] * w + xs[2]]) < SAME ||
                 dist(qe, &yuv[(size_t)ys[2] * w + xs[3]]) < SAME))
            {
                o0[x * 2] = o0[x * 2 + 1] = o1[x * 2] = o1[x * 2 + 1] = ce;
                continue;
            }
            Px A1 = P(-1, -2), B1 = P(0, -2), C1 = P(1, -2);
            Px A0 = P(-2, -1), A = P(-1, -1), B = P(0, -1), C = P(1, -1), C4 = P(2, -1);
            Px D0 = P(-2, 0), D = P(-1, 0), E = P(0, 0), F = P(1, 0), F4 = P(2, 0);
            Px G0 = P(-2, 1), G = P(-1, 1), H = P(0, 1), I = P(1, 1), I4 = P(2, 1);
            Px G5 = P(-1, 2), H5 = P(0, 2), I5 = P(1, 2);
#undef P
            uint32_t e0 = E.c, e1 = E.c, e2 = E.c, e3 = E.c;
            /* bottom-right, bottom-left, top-left, top-right: rotated by 90 degrees each */
            corner(E, F, H, I, C, G, F4, H5, I4, I5, B, D, &e3, &e1, &e2);
            corner(E, H, D, G, I, A, H5, D0, G5, G0, F, B, &e2, &e3, &e0);
            corner(E, D, B, A, G, C, D0, B1, A0, A1, H, F, &e0, &e2, &e1);
            corner(E, B, F, C, A, I, B1, F4, C1, C4, D, H, &e1, &e0, &e3);
            o0[x * 2] = e0;
            o0[x * 2 + 1] = e1;
            o1[x * 2] = e2;
            o1[x * 2 + 1] = e3;
        }
    }
}

void xbr2x(const uint32_t *src, int w, int h, size_t pitch_px, uint32_t *dst)
{
    size_t need = (size_t)w * h;
    if (yuv_size < need)
    {
        free(yuv);
        yuv = malloc(need * sizeof(Yuv));
        yuv_size = yuv ? need : 0;
        if (!yuv)
            return;
    }
    Ctx c = {src, w, h, pitch_px, dst};
    blit_parallel(convert_rows, &c, h);
    blit_parallel(scale_rows, &c, h);
}
