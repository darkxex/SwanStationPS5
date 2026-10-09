/*
 * SwanStationPS5 - platform layer (SDL2 on both PS5 and desktop).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_PLATFORM_H
#define SwanStationPS5_PLATFORM_H

#include "../SwanStationPS5.h"

bool plat_init(void);
const char *plat_init_error(void); /* "SDL window failed: <reason>" after plat_init fails */
/* How frames reach the screen, for the log (plat_init runs before the log opens). */
const char *plat_screen_info(void);
void plat_shutdown(void);

/* PS5: asks the HEN to let SwanStationPS5 list /data. Call before plat_init, while the
 * process is single-threaded. Returns false (with the reason) when SwanStationPS5 stays
 * sandboxed: files still open and save, but folders can't be listed, so the
 * library comes from the index the sync tool writes. Desktop: always true. */
bool plat_prepare_storage(char *error, size_t size);
/* "sandboxed read ok, write no, list no": measured before unlocking (PS5). */
const char *plat_sandbox_probe(void);
/* Settings > System > Unlock /data with etaHEN (PS5); takes effect next launch. */
bool plat_unlock_disabled(void);
void plat_set_unlock_disabled(bool disabled);
void plat_default_root(char *out, size_t size);

/* Reads controllers. `quit` is set when the desktop window is closed. */
void plat_poll(PadState pads[SwanStationPS5_MAX_PADS], bool *quit);
void plat_rumble(int port, uint16_t strong, uint16_t weak);
/* The controller's light bar, 0xRRGGBB (ignored where unsupported). */
void plat_set_lightbar(int port, uint32_t rgb);
/* Players: order[player] is the controller that plays it. Ports everywhere
 * (plat_poll, rumble, light bar, triggers) are players. */
void plat_set_player_order(const int order[SwanStationPS5_MAX_PADS]);
void plat_player_order(int order[SwanStationPS5_MAX_PADS]);
const char *plat_pad_name(int port); /* the PS5 user holding it, or the controller's name */
/* DualSense extras; nothing happens where they aren't supported */
void plat_pad_motion(bool on); /* PadState.quat while on */
void plat_pad_touch(bool on);  /* PadState.touching / touch_x / touch_y while on */
typedef struct
{
    uint8_t mode;       /* TRIGGER_* (Sony's trigger effect modes) */
    uint8_t a, b, c, d; /* feedback: position 0-9, strength 0-8; weapon: start 2-7, end, strength;
                           vibration: position, amplitude 0-8, frequency Hz; slope: start, end,
                           start strength 1-8, end strength 1-8 */
} PlatTrigger;
enum
{
    TRIGGER_OFF = 0,
    TRIGGER_FEEDBACK = 1,
    TRIGGER_WEAPON = 2,
    TRIGGER_VIBRATION = 3,
    TRIGGER_SLOPE = 5
};
void plat_pad_triggers(int port, PlatTrigger l2, PlatTrigger r2);
int plat_pad_battery(int port); /* 0..100, -1 when unknown */
/* The game picture's colours: Settings brightness (0..3, 1 normal), colour (0..5),
 * and sharpening (0..2, Beetle on the GPU). */
void plat_set_colour(int brightness, int colour, int sharpen);
/* The console's time of day as the user set it up ("21:42" or "9:42 PM"). */
void plat_clock(char *out, size_t size);
/* Frame profiling: the time since the previous mark goes to `name`
 * (drawing flushed first); averages are logged every 120 frames. */
void plat_profile(const char *name);
/* A full-screen grey picture (w x h, one byte a pixel) times a colour,
 * as the background: written straight into the canvas on the PS5. */
void plat_draw_backdrop(const uint8_t *grey, int w, int h, uint32_t tint_argb);

/* Audio: interleaved signed 16-bit stereo. */
bool plat_audio_open(int sample_rate);
void plat_audio_close(void);
void plat_audio_push(const int16_t *frames, size_t frame_count);
size_t plat_audio_queued_frames(void);
void plat_audio_clear(void);

/* Drawing in output pixels (1920x1080 on PS5). */
int plat_width(void);
int plat_height(void);
void plat_begin_frame(uint32_t clear_argb);
/* pixel_format: 0 = 0RGB1555, 1 = XRGB8888, 2 = RGB565 (libretro numbering). */
/* upscale 1..4 prescales 32-bit frames with `filter` (enum UpscaleFilter) before the final scale. */
void plat_upload_game(const void *pixels, int width, int height, size_t pitch, int pixel_format,
                      int upscale, int filter);
/* The core rendered this frame on the GPU (v2): the screen draws it. */
void plat_upload_game_gpu(int width, int height);
void plat_draw_game(const Settings *settings, float display_aspect, uint8_t dim);
void plat_fill_rect(int x, int y, int w, int h, uint32_t argb);
/* Where plat_draw_game last put the picture (for borders around it). */
void plat_game_rect(int *x, int *y, int *w, int *h);
void plat_end_frame(void);

/* Textures and textured triangle meshes: the UI (covers in perspective,
 * reflections, gradients, TrueType text) is built from these. */
typedef struct PlatTexture PlatTexture;
typedef struct
{
    float x, y;    /* output pixels */
    float u, v;    /* 0..1 */
    uint32_t argb; /* vertex colour, multiplied with the texture */
} PlatVertex;

PlatTexture *plat_texture_create(const uint8_t *rgba, int width, int height, bool smooth);
void plat_texture_free(PlatTexture *texture);
void plat_texture_size(const PlatTexture *texture, int *width, int *height);
/* texture may be NULL for flat-coloured geometry. indices may be NULL for a plain triangle list. */
void plat_draw_mesh(PlatTexture *texture, const PlatVertex *vertices, int vertex_count,
                    const int *indices, int index_count);
/* Copies a whole texture into a rectangle: one blit, no seams. Much cheaper
 * than a mesh with the software renderer the PS5 build uses. */
void plat_draw_texture(PlatTexture *texture, float x, float y, float w, float h, uint32_t tint,
                       bool blend);
/* Part of a texture (sx, sy, sw, sh in texels) into a rectangle, blended and
 * tinted: glyphs, icons and rounded corners. Takes the renderer's blit path. */
void plat_draw_texture_region(PlatTexture *texture, int sx, int sy, int sw, int sh, float x,
                              float y, float w, float h, uint32_t tint);
/* A blended solid rectangle (the fill path, not triangles). */
void plat_fill_rectf(float x, float y, float w, float h, uint32_t argb);
/* Clip drawing to a rectangle (w <= 0 turns clipping off). */
void plat_set_clip(int x, int y, int w, int h);
/* Full-screen-relative path to packaged read-only assets (/app0/assets on PS5). */
void plat_asset_path(char *out, size_t size, const char *relative);

uint64_t plat_ticks_us(void);
void plat_sleep_us(uint32_t us);
void plat_notify(const char *message); /* PS5 system notification; desktop log */

#endif
