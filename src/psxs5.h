/*
 * PSXS5 - PlayStation X Super 5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared types for the frontend. Everything here is plain C11 so the same
 * sources build for the PS5 (no full libc++) and for the desktop test build.
 */
#ifndef PSXS5_H
#define PSXS5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSXS5_NAME "PSXS5"
#define PSXS5_TITLE_ID "PPSA97510"
#define PSXS5_VERSION "2.1.1"
#define PSXS5_PATH_MAX 512

/* Pad bits use RetroPad numbering so the host can hand the mask to the core
 * unchanged. PlayStation names: Cross = B, Circle = A, Square = Y, Triangle = X. */
enum
{
    BTN_CROSS = 0,
    BTN_SQUARE = 1,
    BTN_SELECT = 2,
    BTN_START = 3,
    BTN_UP = 4,
    BTN_DOWN = 5,
    BTN_LEFT = 6,
    BTN_RIGHT = 7,
    BTN_CIRCLE = 8,
    BTN_TRIANGLE = 9,
    BTN_L1 = 10,
    BTN_R1 = 11,
    BTN_L2 = 12,
    BTN_R2 = 13,
    BTN_L3 = 14,
    BTN_R3 = 15,
    BTN_MENU = 16, /* touchpad click: opens the PSXS5 in-game menu */
};
#define BIT(b) (1u << (b))
#define PSXS5_MAX_PADS 4 /* 3 and 4 play through a multitap */

typedef struct
{
    bool connected;
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t l2, r2;    /* how far the triggers are pressed, 0..255 */
    bool motion;       /* quat holds the controller's orientation */
    float quat[4];     /* x, y, z, w */
    /* the light gun (filled in by PSXS5): -32767..32767 across the picture */
    int16_t gun_x, gun_y;
    bool gun_offscreen;
    /* the touchpad (one finger) and, for mouse games, the pointer's move this frame */
    bool touching;
    uint16_t touch_x, touch_y; /* 0..1919, 0..1079 */
    int16_t mouse_dx, mouse_dy;
} PadState;

enum AspectMode
{
    ASPECT_AUTO = 0, /* what the game/core reports (normally 4:3) */
    ASPECT_4_3,
    ASPECT_16_9,     /* pair with a widescreen cheat for true widescreen */
    ASPECT_16_10,
    ASPECT_PIXEL,    /* 1:1 square pixels */
    ASPECT_STRETCH,  /* fill the whole screen */
    ASPECT_COUNT
};

enum UpscaleFilter
{
    UPSCALE_SHARP = 0, /* nearest-neighbour prescale: crisp pixels */
    UPSCALE_SMOOTH_PIXELS, /* Scale2x/Scale3x edge smoothing */
    UPSCALE_XBR,           /* xBR: edge-directed, smoothest (2x passes) */
    UPSCALE_FILTER_COUNT
};

enum RegionMode
{
    REGION_AUTO = 0,
    REGION_NTSC,
    REGION_PAL,
    REGION_COUNT
};

typedef struct
{
    int aspect;       /* enum AspectMode */
    bool integer_scale; /* whole-number scale factors only */
    bool smooth;      /* bilinear for the final scale to the screen */
    int internal_res; /* 1..5 = native, 2x, 4x, 8x, 16x; PCSX-ReARMed stops at 2x */
    int upscale;      /* 1..4: prescale before the final scale */
    int upscale_filter; /* enum UpscaleFilter */
    bool show_fps;
    int region;       /* enum RegionMode */
    bool force_hle;   /* ignore BIOS files and use the built-in HLE BIOS */
    bool dithering;
    bool cd_fast;     /* faster CD reads (shorter loads, rare glitches) */
    bool analog;      /* DualShock instead of digital pad */
    int state_slot;   /* 0..9 */
    int last_game;    /* library cursor */
    int cover_style;  /* enum CoverStyle */
    bool cover_download; /* fetch missing covers over the network */
    int ui_sound;     /* SFX_STYLE_* in ui/sfx.h; SFX_STYLE_OFF mutes */
    int ui_volume;    /* 0..3 = 25/50/75/100 % */
    int stick_dpad;   /* enum StickDpad */
    int language;     /* enum Lang in i18n.h */
    bool rumble;          /* controller vibration */
    int rumble_strength;  /* 0..3 = 25/50/75/100 % */
    /* Button mapping: for each controller button (BTN_CROSS..BTN_R3), the PS1
     * button it presses (BTN_*), or -1 for nothing. */
    int8_t button_map[16];
    int sort_mode;        /* shelf order, enum SortMode in ui/coverflow.h */
    int shelf_category;   /* shelf filter, enum ShelfCategory in ui/coverflow.h */
    int background;       /* 0 dark, 1 the selected cover's colour */
    bool widescreen;      /* turn on the game's widescreen code, show 16:9 */
    bool multitap;        /* 4 players through a multitap in port 1 */
    bool rewind;          /* keep the last seconds for rewinding */
    bool quick_resume;    /* save on quitting, offer Continue on the shelf */
    int crt;              /* scanlines: 0 off, 1 light, 2 strong */
    int border;           /* around the picture: 0 black, 1 glow, 2 TV frame */
    bool remote;          /* settings page for phones on the local network */
    bool update_check;    /* look for new PSXS5 releases at start */
    int emulator;         /* enum Emulator */
    bool pgxp;            /* Beetle: precise geometry, no wobbling polygons */
    int shader;           /* Beetle on the GPU: 0 off, 1 sharp bilinear, 2 CRT */
    bool hd_textures;     /* Beetle: use a texture pack beside the game when there is one */
    int theme;            /* enum ThemeId in ui/theme.h */
    int crop_edges;       /* 0 off, 1: 8 lines top and bottom, 2: 16 (the black a CRT hid) */
    bool ra_popups;       /* achievement unlock banners while playing */
    bool ra_tracker;      /* the progress card when a counted achievement moves */
    int ra_popup_style;   /* 0 banner, 1 compact, 2 big trophy */
    int lightbar;         /* 0 left to the system, 1 player colours, 2 the game's cover colour */
    int stick_deadzone;   /* 0 off, 1..4: 5, 10, 15, 20 % of the stick's travel ignored */
    int stick_response;   /* 0 normal, 1 precise (slow near the centre), 2 quick */
    bool trigger_effects; /* DualSense: pedal feel in racing games, a trigger click for light guns */
    bool racing_triggers; /* racing games: R2 is the gas (Cross), L2 the brake (Square) */
    int rumble_feel;      /* 0 classic, 1 soft, 2 punchy, 3 punchy and in the triggers */
    int lightgun;         /* 0 automatic (known gun games), 1 off, 2 on */
    int overclock;        /* 0 off, 1 a little, 2 a lot */
    int brightness;       /* 0 darker, 1 normal, 2 brighter, 3 brightest */
    int colour;           /* 0 natural, 1 vivid, 2 soft, 3 warm, 4 cool, 5 black and white */
    int autosave;         /* 0 off, else every 5, 10 or 15 minutes into the auto slots */
    bool bezel;           /* the game's artwork around a 4:3 picture, when downloaded */
    int msaa;             /* Beetle on the GPU: anti-aliasing 0 off, 1..4 = 2x, 4x, 8x, 16x */
    int texture_filter;   /* Beetle: 0 off, 1 bilinear, 2 xBR, 3 SABR, 4 JINC2, 5 3-point */
    bool filter_2d;       /* filter 2D sprites and menus too (off keeps them sharp) */
    bool supersampling;   /* Beetle: render at the internal resolution, then scale down */
    int deinterlace;      /* Beetle: 0 weave, 1 bob, 2 motion-adaptive */
    bool pal60;           /* Beetle: European games at 60 Hz */
    bool game_fixes;      /* turn off what DuckStation's database says a game breaks with */
    bool fmv_smooth;      /* Beetle: smooth the colour blocks of FMVs (MDEC chroma filter) */
    bool true_colour;     /* Beetle: 32-bit colour, no dithering */
    bool boot_intro;      /* the PS1's startup logo and sound before the game (a real BIOS) */
    int sharpen;          /* Beetle on the GPU: 0 off, 1 light, 2 strong (AMD FidelityFX CAS) */
    bool disc_animation;  /* the disc slides out of its case when a game starts */
    bool negcon;          /* racing games that take a NeGcon: analog gas and brake on R2 / L2 */
    bool touch_mouse;     /* mouse games: the touchpad moves the pointer */
    int run_ahead;        /* 0 off, 1 or 2 frames less input lag */
    bool fast_effects;    /* Beetle on the GPU: screen effects on the GPU, without the software copy */
} Settings;

enum Emulator
{
    EMU_AUTO,   /* Beetle when it can run the game, else PCSX-ReARMed */
    EMU_PCSX,
    EMU_BEETLE,
    EMU_SWANSTATION,
    EMU_COUNT
};

enum StickDpad
{
    STICK_DPAD_AUTO = 0, /* left stick drives the D-pad while the game uses digital mode */
    STICK_DPAD_ALWAYS,
    STICK_DPAD_OFF,
    STICK_DPAD_COUNT
};

enum CoverStyle
{
    COVER_FLAT = 0, /* front art, shown in perspective by PSXS5 */
    COVER_BOX3D,    /* pre-rendered 3D jewel case */
    COVER_STYLE_COUNT
};

/* Data layout below the PSXS5 root (default /data/PSXS5 on PS5). */
typedef struct
{
    char root[PSXS5_PATH_MAX];
    char games[PSXS5_PATH_MAX];
    char bios[PSXS5_PATH_MAX];
    char saves[PSXS5_PATH_MAX];
    char states[PSXS5_PATH_MAX];
    char cheats[PSXS5_PATH_MAX];
    char covers[PSXS5_PATH_MAX]; /* covers/default/<serial>.jpg, covers/3d/<serial>.png */
    char logs[PSXS5_PATH_MAX];
    char config[PSXS5_PATH_MAX];
    char user[PSXS5_PATH_MAX];   /* the profile's folder (saves, states, settings, stats): root for the main one */
} Paths;

void psxs5_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void psxs5_log_open(const char *path);

/* Small string helpers shared by the frontend. */
void str_copy(char *dst, size_t size, const char *src);
void path_join(char *dst, size_t size, const char *a, const char *b);
bool path_exists(const char *path);
bool path_is_dir(const char *path);
bool make_dirs(const char *path);
bool file_copy(const char *from, const char *to); /* replaces `to` */
const char *path_ext(const char *path);  /* lower-case-insensitive extension without dot, "" if none */
int str_icmp(const char *a, const char *b);
/* localtime into *out (the PS5 libc has no localtime_r); false on failure */
struct tm;
bool local_time(long long when, struct tm *out);

#endif
