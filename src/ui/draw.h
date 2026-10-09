/*
 * PSXS5 - shape helpers built on plat_draw_mesh.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSXS5_DRAW_H
#define PSXS5_DRAW_H

#include "../psxs5.h"

enum PadGlyph
{
    GLYPH_CROSS,
    GLYPH_CIRCLE,
    GLYPH_SQUARE,
    GLYPH_TRIANGLE,
    /* the rest of the pad, for hints and the button-mapping screen */
    GLYPH_UP,
    GLYPH_DOWN,
    GLYPH_LEFT,
    GLYPH_RIGHT,
    GLYPH_L1,
    GLYPH_R1,
    GLYPH_L2,
    GLYPH_R2,
    GLYPH_L3,
    GLYPH_R3,
    GLYPH_START,    /* OPTIONS on a DualSense */
    GLYPH_SELECT,
    GLYPH_TOUCHPAD,
    GLYPH_NONE,     /* "not mapped" */
    GLYPH_PS1_START, /* the PS1 pad's START (OPTIONS is the DualSense's) */
    GLYPH_COUNT
};

void draw_rect(float x, float y, float w, float h, uint32_t argb);
/* Vertical gradient through `count` stops at positions 0..1. */
void draw_vgradient(float x, float y, float w, float h, const uint32_t *colors,
                    const float *stops, int count);
void draw_line(float x0, float y0, float x1, float y1, float thickness, uint32_t argb);
void draw_ring(float cx, float cy, float radius, float thickness, uint32_t argb);
/* Soft halo around a rectangle: `spread` pixels fading from `argb` to transparent. */
void draw_glow(float x, float y, float w, float h, float spread, uint32_t argb);
/* Draws the glyph centred on cx, cy; returns its width (wider for L1, OPTIONS...). */
float draw_pad_glyph(enum PadGlyph glyph, float cx, float cy, float size);
/* Width draw_pad_glyph will use. */
float pad_glyph_width(enum PadGlyph glyph, float size);
/* "[glyph] label" button hint; returns the width used. */
float draw_hint(float x, float y, enum PadGlyph glyph, const char *label, float size,
                uint32_t argb);

/* Several glyphs joined by `sep` ('+' held together, '/' alternatives) and a label: "[L1]/[R1] label".
 * hint_combo_width gives the width draw_hint_combo will use. */
float hint_combo_width(const int *glyphs, int count, const char *label, float size);
float draw_hint_combo(float x, float y, const int *glyphs, int count, char sep, const char *label,
                      float size, uint32_t argb);

uint32_t argb_alpha(uint32_t argb, float alpha); /* scales the alpha channel */
uint32_t argb_lerp(uint32_t a, uint32_t b, float t);

/* Rounded shapes, built from fills and a cached circle texture (fast path). */
void draw_rrect(float x, float y, float w, float h, float radius, uint32_t argb);
void draw_rrect_outline(float x, float y, float w, float h, float radius, float thickness,
                        uint32_t argb);
void draw_circle(float cx, float cy, float radius, uint32_t argb);
/* An on/off switch; t animates 0 (off) .. 1 (on). Width is 2.1 x height. */
void draw_switch(float x, float y, float h, float t);
/* Pill with text; returns its width. */
float draw_pill(float x, float y, float h, float text_size, uint32_t fill, uint32_t text_argb,
                const char *text);
/* "< value >" ending at right_x (chevrons are icons); returns its width. */
float draw_choice(float right_x, float y, float h, float text_size, uint32_t fill, uint32_t text_argb,
                  const char *text);
/* Animated value: v moves towards target at TH_SNAP-like `speed` per second. */
void anim_approach(float *v, float target, float dt, float speed);
/* A scrolling list's first row: moved only as far as needed to show `cursor`,
 * never past the end. Keep it as a whole number and animate towards it. */
int list_top_row(int top, int cursor, int rows, int count);

#endif
