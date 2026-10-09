/*
 * SwanStationPS5 - TrueType text (Inter) on top of the platform mesh API.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_TEXT_H
#define SwanStationPS5_TEXT_H

#include "../SwanStationPS5.h"

enum
{
    FONT_REGULAR = 0,
    FONT_BOLD = 1,
    FONT_MONO = 2, /* IBM Plex Mono: text guides keep their columns */
};

enum
{
    ALIGN_LEFT = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT = 2,
};

bool text_init(void);
void text_shutdown(void);
void text_language_changed(void); /* repacks glyphs for the new language */
/* Another pair of faces (assets paths), for the themes. */
void text_set_fonts(const char *regular, const char *bold);

/* (x, y) is the top-left of the line box for ALIGN_LEFT; size is the pixel height. */
void text_draw(float x, float y, float size, int weight, uint32_t argb, int align, const char *s);
float text_width(float size, int weight, const char *s);
/* Truncates with "..." so the text fits in max_width. */
void text_draw_fit(float x, float y, float size, int weight, uint32_t argb, int align,
                   float max_width, const char *s);

/* CPU text into an RGBA image, word-wrapped to max_width, centred per line.
 * Used to make cover placeholders. Returns the height used. */
int text_render_rgba(uint8_t *rgba, int width, int height, int y, float size, int weight,
                     uint32_t argb, int max_width, const char *s);

#endif
