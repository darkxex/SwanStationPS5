/*
 * SwanStationPS5 v2 - the screen through Vulkan (RADV, VK_KHR_display on VideoOut).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_VK_PRESENT_H
#define SwanStationPS5_VK_PRESENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Takes over the TV through Vulkan. The interface keeps drawing on the CPU
 * into a canvas_w x canvas_h RGBA canvas, which is uploaded every frame. */
bool vkp_open(int canvas_w, int canvas_h, char *error, size_t size);
/* Uploads the canvas (R,G,B,A bytes), draws it over the game picture (none
 * yet), presents, and waits for the TV's refresh. */
void vkp_present(const uint32_t *pixels, size_t pitch_bytes);
void vkp_close(void);
/* "1920x1080 @ 59.94 Hz" once open */
const char *vkp_describe(void);
/* A core renders through Vulkan and has handed over a picture. */
bool vkp_game_image_ready(void);
/* Draw that picture in this rectangle of the canvas, under it, this frame. */
/* crop: share of the picture's height hidden at the top and at the bottom */
/* shader: 0 none, 1 sharp bilinear, 2 CRT, 3 supersampling (averaged down); tex_*: the picture's size; lines: the PS1's */
/* The game picture's colours (1, 1, 0: unchanged) and sharpening (0 none .. 1). */
void vkp_set_colour(float brightness, float saturation, float warmth, float sharpen);
/* The final scale of the game picture: nearest (sharp pixels) instead of bilinear. Needs no shader. */
void vkp_set_game_nearest(bool nearest);
void vkp_set_framegen(bool on, double core_hz, double speed, bool nominal); /* Settings > Display > Frame interpolation;
                                                                                the core's rate, the emulation's speed (%), and whether it is normal */
void vkp_set_fsr(bool on);            /* Settings > Display > FSR 1 */
double vkp_framegen_hz(void);         /* the display's rate while frames are being generated, else 0 */
void vkp_frame_repeated(void);        /* the core's last frame repeats the picture of the one before */
void vkp_want_high_refresh(bool on);  /* before vkp_open: the 120 Hz mode */
bool vkp_set_mode(int width, int height, char *error, size_t size); /* the TV's mode, at once (with the GPU interface); the screen's objects are made again */
void vkp_want_size(int width, int height); /* before vkp_open: the screen's mode (the canvas's size when the TV has none like it) */
void vkp_show_game(float x, float y, float w, float h, float crop, int shader, int tex_w, int tex_h, int lines);

/* The interface drawn by the GPU instead of the CPU canvas (vk_ui.inc). Coordinates are canvas pixels. */
typedef struct
{
    float x, y, u, v;
    uint32_t argb; /* tint, multiplied with the texture */
} VkpUiVertex;
bool vkp_ui_open(char *error, size_t size); /* after vkp_open; false: keep the canvas */
bool vkp_ui_active(void);
int vkp_ui_texture_create(const uint8_t *rgba, int w, int h, bool smooth, bool mips, bool copy); /* -1: failed; copy: else rgba must outlive it */
void vkp_ui_texture_free(int id);
void vkp_ui_texture_update(int id, const uint8_t *rgba);
void vkp_ui_blend(bool on);                 /* the next draws are blended (default) or copied, ignoring the texture's alpha */
void vkp_ui_game_here(void);                /* the game's picture goes here in the list: the border drawn so far is under it */
void vkp_ui_begin(uint32_t clear_argb);     /* a new frame's list; the colour the screen is cleared to under it and the game (alpha 0: black) */
void vkp_ui_clip(int x, int y, int w, int h); /* w <= 0: none */
void vkp_ui_draw(int id, const VkpUiVertex *v, int count); /* triangles; id < 0: no texture (the tint alone) */
void vkp_present_ui(void);                  /* the frame: the game, then the list */

#endif
