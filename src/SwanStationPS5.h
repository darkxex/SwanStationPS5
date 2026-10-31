/*
 * SwanStationPS5 - PlayStation X Super 5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared types for the frontend. Everything here is plain C11 so the same
 * sources build for the PS5 (no full libc++) and for the desktop test build.
 */
#ifndef SwanStationPS5_H
#define SwanStationPS5_H

#include <stdbool.h>
#include "core/bios_choices.h"
#include "core/swanstation_options.h"
#include <stddef.h>
#include <stdint.h>

#define SwanStationPS5_NAME "SwanStationPS5"           /* identifiers: the User-Agent of network requests, the Vulkan application name */
#define SwanStationPS5_TITLE "SwanStationPS5" /* what is shown on screen */
#define SwanStationPS5_TITLE_ID "PPSA98510"
#define SwanStationPS5_VERSION "1.6.2"
#define SwanStationPS5_PATH_MAX 512

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
    BTN_MENU = 16, /* touchpad click: opens the SwanStationPS5 in-game menu */
};
#define BIT(b) (1u << (b))
#define SwanStationPS5_MAX_PADS 4 /* 3 and 4 play through a multitap */

typedef struct
{
    bool connected;
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t l2, r2;    /* how far the triggers are pressed, 0..255 */
    bool motion;       /* quat holds the controller's orientation */
    float quat[4];     /* x, y, z, w */
    /* the light gun (filled in by SwanStationPS5): -32767..32767 across the picture */
    int16_t gun_x, gun_y;
    bool gun_offscreen;
    /* the touchpad (one finger) and, for mouse games, the pointer's move this frame */
    bool touching;
    uint16_t touch_x, touch_y; /* 0..1919, 0..1079 */
    int16_t mouse_dx, mouse_dy;
} PadState;

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
    REGION_NTSC_J, /* the core's order: Auto, NTSC-J, NTSC-U, PAL */
    REGION_NTSC,
    REGION_PAL,
    REGION_COUNT
};

typedef struct
{
    bool integer_scale; /* whole-number scale factors only */
    bool smooth;      /* bilinear for the final scale to the screen */
    int renderer;     /* SwanStation: 0 Vulkan (the GPU), 1 Software (the CPU) */
    int internal_res; /* the core's scale, 1..16 (1 native) */
    int upscale;      /* 1..4: prescale before the final scale */
    int upscale_filter; /* enum UpscaleFilter */
    bool show_fps;
    int region;       /* enum RegionMode */
    bool dithering;
    int cd_speed;     /* CD read speed-up, 1..10 (1 off): shorter loads, rare glitches */
    bool analog;      /* DualShock instead of digital pad */
    int state_slot;   /* 0..9 */
    int last_game;    /* library cursor */
    int cover_style;  /* enum CoverStyle */
    bool cover_download; /* fetch missing covers over the network */
    int ui_sound;     /* SFX_STYLE_* in ui/sfx.h; SFX_STYLE_OFF mutes */
    int ui_volume;    /* 0..3 = 25/50/75/100 % */
    int stick_dpad;   /* enum StickDpad */
    int menu_combo;   /* how the game menu opens: 0 touchpad + L1 + R1 held, 1 L3 + R3 held, 2 Options + touchpad held */
    int menu_hold;    /* how long that combination is held: 0 immediate, 1 0.5 s, 2 1 s */
    int language;     /* enum Lang in i18n.h */
    int vibration;        /* controller vibration: 0 off, 1..4 = 25/50/75/100 % */
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
    bool update_check;    /* look for new SwanStationPS5 releases at start */
    bool pgxp;            /* precise geometry, no wobbling polygons */
    int shader;           /* 0 off, 1 LCD3x, 2 CRT Royale Fast, 3 and 4 retired, 5 to 8 NTSC, 9 to 12 CRT Lottes, CRT Pi, 11 retired, zfast LCD, 13 to 16 CRT Royale, Newpixie, Hyllian NTSC, Mattias */
    bool hd_textures;     /* use a texture pack beside the game when there is one */
    int theme;            /* enum ThemeId in ui/theme.h */
    int crop_edges;       /* 0 off, 1: 8 lines top and bottom, 2: 16 (the black a CRT hid) */
    bool manual_crop;     /* the core's crop mode is ignored (drawn as None) and the picture's area is set by hand (mc_*) */
    int mc_w, mc_h;       /* manual crop: the picture's width and height as a percentage of its normal size (25..150) */
    int mc_x, mc_y;       /* manual crop: how far the picture is moved, in thousandths of the screen (-500..500) */
    bool ra_popups;       /* achievement unlock banners while playing */
    bool ra_tracker;      /* the progress card when a counted achievement moves */
    int ra_popup_style;   /* 0 banner, 1 compact, 2 big trophy */
    int bios_start;       /* Start BIOS boots this region's BIOS: 0 NTSC-U (default), 1 NTSC-J, 2 PAL; not saved */
    int lightbar;         /* 0 left to the system, 1 player colours, 2 the game's cover colour, 3 red in analog mode / off in digital */
    int stick_deadzone;   /* 0 off, 1..4: 5, 10, 15, 20 % of the stick's travel ignored */
    int stick_response;   /* 0 normal, 1 precise (slow near the centre), 2 quick */
    int trigger_effects;  /* DualSense triggers: 0 off, 1 automatic (pedal / gun click), then one feel for every game: click, light click, firm, buzz */
    bool racing_triggers; /* R2 and Cross swap places, and L2 and Square (off by default) */
    int rumble_feel;      /* 0 classic, 1 soft, 2 punchy, 3 punchy and in the triggers */
    int lightgun;         /* 0 automatic (known gun games), 1 off, 2 on */
    int overclock;        /* 0 off, 1 a little, 2 a lot */
    int brightness;       /* 0 darker, 1 normal, 2 brighter, 3 brightest */
    int colour;           /* 0 natural, 1 vivid, 2 soft, 3 warm, 4 cool, 5 black and white */
    int autosave;         /* 0 off, else every 5, 10 or 15 minutes into the auto slots */
    bool bezel;           /* the game's artwork around a 4:3 picture, when downloaded */
    int msaa;             /* anti-aliasing 0 off, 1..4 = 2x, 4x, 8x, 16x */
    int texture_filter;   /* 0 off, 1 bilinear, 2 xBR, 3 SABR, 4 JINC2, 5 3-point */
    bool filter_2d;       /* filter 2D sprites and menus too (off keeps them sharp) */
    bool supersampling;   /* render at the internal resolution, then scale down */
    int deinterlace;      /* 0 weave, 1 bob, 2 motion-adaptive */
    bool pal60;           /* European games at 60 Hz */
    bool game_fixes;      /* turn off what DuckStation's database says a game breaks with */
    bool fmv_smooth;      /* smooth the colour blocks of FMVs (MDEC chroma filter) */
    bool true_colour;     /* 32-bit colour, no dithering */
    bool boot_intro;      /* the PS1's startup logo and sound before the game (a real BIOS) */
    int sharpen;          /* 0 off, 1 light, 2 strong (AMD FidelityFX CAS) */
    bool disc_in_tray;    /* the disc spins flat on its tray, without lifting or growing, as in 1.3.7 */
    bool disc_animation;  /* the disc slides out of its case when a game starts */
    bool disc_ask; /* a disc put in opens its question by itself; off: only when the game is pressed on the shelf */
    bool ntfs_enabled; /* the NTFS disks plugged in are listed under /ntfs and their games on the shelf */
    bool restart_after_game; /* closing a game starts the title again (gives the game's memory back); off: the library stays in the same process */
    bool install_to_ext; /* a disc is installed on the first extended storage with room (1 GB free) instead of /data */
    bool case_lighting;   /* the cover in the jewel case gets light: its shading and the glare over the lid; off: the picture as it is */
    bool jewel_cases;     /* the shelf shows 3D jewel cases; off: the flat covers with a reflection, as in 1.1.8 */
    bool negcon;          /* racing games that take a NeGcon: analog gas and brake on R2 / L2 */
    bool touch_mouse;     /* mouse games: the touchpad moves the pointer */
    int run_ahead;        /* 0 off, 1 to 10 frames less input lag */
    bool fast_effects;    /* screen effects on the GPU, without the software copy */
    bool fsr;             /* FSR 1 scales the picture before the shader */
    char usb_folder[48];  /* a folder name looked for on USB drives: /mnt/ext0/<name>, /mnt/ext1/<name> and /mnt/usb0/<name> to /mnt/usb7/<name> (empty: none) */
    int nfs_timeout;  /* how long an NFS read may take before the connection is given up: 0..4 = 5, 10, 15, 20, 30 s (2 default) */
    char nfs_shares[512]; /* nfs:// addresses of the servers that hold games, ';' between them (empty: none); src/nfs/nfs.h */
    bool nostalgia;       /* while a game is played: no sound of the app, no notices or trophies, and the game is silent */
    bool frame_pacing;    /* Video sync: one game frame per screen refresh, the sound stretched by a hair to keep up; off: synchronised to the sound */
    int video_out;        /* picture output to the TV: 0 60 Hz, 1 120 Hz (from the next start) */
    int framegen;         /* generated frames: 0 off, 1 one between two of the game's (x2), 2 three (x4, a 30 fps game on a 120 Hz screen) */
    int aspect_before_wide; /* the aspect ratio (index) the widescreen hack replaced with 16:9, or -1; saved by value */
    int ss_opt[SS_OPT_COUNT]; /* SwanStation's own core options: the chosen value's index in SS_OPTS[i].values */
} Settings;

enum StickDpad
{
    STICK_DPAD_AUTO = 0, /* left stick drives the D-pad while the game uses digital mode */
    STICK_DPAD_ALWAYS,
    STICK_DPAD_OFF,
    STICK_DPAD_COUNT
};

enum CoverStyle
{
    COVER_FLAT = 0, /* front art, shown in perspective by SwanStationPS5 */
    /* COVER_BOX3D,    pre-rendered 3D jewel case (no longer used) */
    COVER_STYLE_COUNT
};

/* Data layout below the SwanStationPS5 root (default /data/SwanStationPS5 on PS5). */
typedef struct
{
    char root[SwanStationPS5_PATH_MAX];
    char games[SwanStationPS5_PATH_MAX];
    char bios[SwanStationPS5_PATH_MAX];
    char saves[SwanStationPS5_PATH_MAX];
    char states[SwanStationPS5_PATH_MAX];
    char cheats[SwanStationPS5_PATH_MAX];
    char covers[SwanStationPS5_PATH_MAX]; /* covers/default/<serial>.jpg, covers/3d/<serial>.png */
    char logs[SwanStationPS5_PATH_MAX];
    char cache[SwanStationPS5_PATH_MAX];  /* downloads and the emulator's shader cache (cache/swanstation) */
    char config[SwanStationPS5_PATH_MAX];
    char user[SwanStationPS5_PATH_MAX];   /* the profile's folder (saves, states, settings, stats): root for the main one */
} Paths;

void SwanStationPS5_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void SwanStationPS5_log_open(const char *path);

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
