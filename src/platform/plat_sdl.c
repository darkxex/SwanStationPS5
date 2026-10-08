/*
 * PSXS5 - SDL2 platform layer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The same file drives the PS5 build (PacBrew SDL2 port) and the desktop
 * test build, so emulator behaviour can be debugged on a PC first.
 */
#include "platform.h"

#include "xbr.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__PROSPERO__)
#include "blit.h"
#include "ps5_unlock.h"
#include "ps5_video.h"
#include "vk/vk_present.h"
#include <sys/mman.h>
int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);
int sceSystemServiceHideSplashScreen(void);
int psxs5_elevate(const char **route); /* elevation_shim.cpp */
#else
static bool init_desktop_window(void);
#if defined(PSXS5_PREVIEW)
static bool init_preview_screen(void);
#endif
#endif


#define GAME_TEX_W 1024
#define GAME_TEX_H 512

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *game_texture;
static int game_texture_format = -1;
static int game_w = 320, game_h = 240;
static int out_w = 1920, out_h = 1080;
static SDL_AudioDeviceID audio_device;
static int audio_rate;

static SDL_GameController *controllers[PSXS5_MAX_PADS];
static uint16_t rumble_strong[PSXS5_MAX_PADS], rumble_weak[PSXS5_MAX_PADS];

static void set_draw_color(uint32_t argb)
{
    SDL_SetRenderDrawColor(renderer, (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff,
                           argb >> 24);
}

static char init_error[256];

const char *plat_init_error(void)
{
    return init_error;
}

static char screen_info[256] = "SDL window";

const char *plat_screen_info(void)
{
    return screen_info;
}

static bool init_failed(const char *stage)
{
    snprintf(init_error, sizeof(init_error), "%s failed: %s", stage, SDL_GetError());
    psxs5_log("%s", init_error);
    return false;
}

#if defined(__PROSPERO__)
static SDL_Surface *canvas; /* 1920x1080 RGBA in ordinary memory; shown by ps5_video */
static bool use_vulkan;    /* v2: shown by vk_present instead */

/* SDL's PS5 video driver can't provide a window surface or a renderer, so on
 * PS5 SDL is started without it: SDL draws (software renderer into `canvas`),
 * plays sound and reads controllers; ps5_video.c puts frames on screen. */
static bool init_ps5_screen(void)
{
    char error[160];
#if defined(PSXS5_VULKAN)
    /* v2: the screen through Vulkan; the old VideoOut path stays as the
     * fallback (and is forced by creating /data/PSXS5/no_vulkan). */
    /* fopen, not access(): access() fails in the sandbox even for files that open */
    FILE *off = fopen("/data/PSXS5/no_vulkan", "rb");
    bool forced_off = off != NULL;
    if (off)
        fclose(off);
    use_vulkan = !forced_off && vkp_open(PS5_SCREEN_W, PS5_SCREEN_H, error, sizeof(error));
    if (use_vulkan)
        snprintf(screen_info, sizeof(screen_info), "Vulkan, %s", vkp_describe());
    else
        snprintf(screen_info, sizeof(screen_info), "VideoOut (Vulkan %s)",
                 forced_off ? "turned off by /data/PSXS5/no_vulkan" : error);
    if (!use_vulkan && !ps5_video_open(error, sizeof(error)))
#else
    snprintf(screen_info, sizeof(screen_info), "VideoOut");
    if (!ps5_video_open(error, sizeof(error)))
#endif
    {
        snprintf(init_error, sizeof(init_error), "screen: %s", error);
        return false;
    }
    const size_t bytes = (size_t)PS5_SCREEN_W * PS5_CANVAS_ROWS * 4; /* zero-filled padding rows */
    void *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (pixels == MAP_FAILED)
    {
        snprintf(init_error, sizeof(init_error), "screen: no memory for the canvas");
        return false;
    }
    canvas = SDL_CreateRGBSurfaceWithFormatFrom(pixels, PS5_SCREEN_W, PS5_SCREEN_H, 32,
                                                PS5_SCREEN_W * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!canvas)
        return init_failed("SDL canvas");
    renderer = SDL_CreateSoftwareRenderer(canvas);
    if (!renderer)
        return init_failed("SDL software renderer");
    return true;
}
#endif

bool plat_init(void)
{
#if defined(__PROSPERO__)
    sceSystemServiceHideSplashScreen();
    if (SDL_Init(SDL_INIT_EVENTS) != 0)
        return init_failed("SDL");
#else
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return init_failed("SDL video");
#endif
    /* Sound or controller trouble must not stop the app. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        psxs5_log("SDL audio unavailable: %s", SDL_GetError());
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        psxs5_log("SDL controllers unavailable: %s", SDL_GetError());

#if defined(__PROSPERO__)
    if (!init_ps5_screen())
        return false;
#elif defined(PSXS5_PREVIEW)
    if (!init_preview_screen())
        return false;
#else
    if (!init_desktop_window())
        return false;
#endif
    out_w = 1920;
    out_h = 1080;
    SDL_RenderSetLogicalSize(renderer, out_w, out_h);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) == 0)
        psxs5_log("renderer: %s", info.name);

    SDL_GameControllerEventState(SDL_ENABLE);
    return true;
}

#if defined(PSXS5_PREVIEW)
/* ---- interface preview (tools/ui-preview): the PS5's software renderer on an
 * off-screen surface, a fixed 60 Hz clock, scripted buttons and screenshots. */
static SDL_Surface *preview_surface;
static uint64_t preview_clock;
static FILE *preview_script;
static uint32_t preview_buttons;
static int preview_hold;
static bool preview_quit, preview_release;

static void preview_step(void)
{
    /* script lines: "wait N", "press BUTTONS [N]", "hold BUTTONS N", "shot NAME", "quit"
     * BUTTONS: names joined by + (cross circle square triangle up down left right
     * l1 r1 l2 r2 l3 r3 start select menu) */
    static const struct { const char *name; int bit; } names[] = {
        {"cross", BTN_CROSS}, {"circle", BTN_CIRCLE}, {"square", BTN_SQUARE},
        {"triangle", BTN_TRIANGLE}, {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT},
        {"right", BTN_RIGHT}, {"l1", BTN_L1}, {"r1", BTN_R1}, {"l2", BTN_L2}, {"r2", BTN_R2},
        {"l3", BTN_L3}, {"r3", BTN_R3}, {"start", BTN_START}, {"select", BTN_SELECT},
        {"menu", BTN_MENU}};
    if (preview_hold > 0)
    {
        --preview_hold;
        return;
    }
    if (preview_release)
    {
        /* one frame with nothing held between commands, so presses register */
        preview_release = false;
        preview_buttons = 0;
        return;
    }
    preview_buttons = 0;
    char line[256];
    while (preview_script && fgets(line, sizeof(line), preview_script))
    {
        char cmd[32] = "", arg[200] = "";
        int n = 1;
        if (sscanf(line, "%31s %199s %d", cmd, arg, &n) < 1 || cmd[0] == '#')
            continue;
        if (!strcmp(cmd, "quit"))
        {
            preview_quit = true;
            return;
        }
        if (!strcmp(cmd, "wait"))
        {
            preview_hold = atoi(arg) - 1;
            return;
        }
        if (!strcmp(cmd, "shot"))
        {
            char path[300];
            snprintf(path, sizeof(path), "%s/%s.bmp", SDL_getenv("PSXS5_SHOTS") ? SDL_getenv("PSXS5_SHOTS") : ".", arg);
            SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1080, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_Rect r = {0, 36, 1920, 1080}; /* the 1080 lines inside the 1152-line canvas */
            SDL_BlitSurface(preview_surface, &r, out, NULL);
            SDL_SaveBMP(out, path);
            SDL_FreeSurface(out);
            psxs5_log("preview: %s", path);
            continue;
        }
        if (!strcmp(cmd, "press") || !strcmp(cmd, "hold"))
        {
            for (char *tok = strtok(arg, "+"); tok; tok = strtok(NULL, "+"))
                for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                    if (!strcmp(tok, names[i].name))
                        preview_buttons |= BIT(names[i].bit);
            /* press: down for one frame, then up for one */
            preview_hold = (!strcmp(cmd, "hold") ? n : 1) - 1;
            preview_release = true;
            return;
        }
    }
}

static bool init_preview_screen(void)
{
    preview_surface = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1152, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!preview_surface)
        return init_failed("preview surface");
    renderer = SDL_CreateSoftwareRenderer(preview_surface);
    if (!renderer)
        return init_failed("preview renderer");
    const char *script = SDL_getenv("PSXS5_SCRIPT");
    preview_script = script ? fopen(script, "r") : NULL;
    return true;
}
#endif

#if !defined(__PROSPERO__)
static bool init_desktop_window(void)
{
    window = SDL_CreateWindow(PSXS5_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window)
        return init_failed("SDL window");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
        return init_failed("SDL renderer");
    return true;
}
#endif

void plat_shutdown(void)
{
    plat_audio_close();
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
        if (controllers[i])
            SDL_GameControllerClose(controllers[i]);
    if (game_texture)
        SDL_DestroyTexture(game_texture);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

void plat_default_root(char *out, size_t size)
{
#if defined(__PROSPERO__)
    str_copy(out, size, "/data/PSXS5");
#else
    const char *env = SDL_getenv("PSXS5_ROOT");
    str_copy(out, size, env && *env ? env : "./psxs5-data");
#endif
}

/* What a still-sandboxed PSXS5 can do with its data folder, for the log. */
static char sandbox_probe[96] = "not probed";

const char *plat_sandbox_probe(void)
{
    return sandbox_probe;
}

/* Settings > System > "Unlock /data with etaHEN": stored as a marker file so
 * it can be read before any unlock (paths that may work while sandboxed). */
static const char *const NO_UNLOCK_MARKERS[] = {"/download0/psxs5_no_unlock",
                                                "/data/PSXS5/no_unlock"};

bool plat_unlock_disabled(void)
{
    for (size_t i = 0; i < 2; ++i)
        if (path_exists(NO_UNLOCK_MARKERS[i]))
            return true;
    return false;
}

void plat_set_unlock_disabled(bool disabled)
{
    for (size_t i = 0; i < 2; ++i)
    {
        if (disabled)
        {
            FILE *f = fopen(NO_UNLOCK_MARKERS[i], "w");
            if (f)
                fclose(f);
        }
        else
            remove(NO_UNLOCK_MARKERS[i]);
    }
}

bool plat_prepare_storage(char *error, size_t size)
{
#if defined(__PROSPERO__)
    {
        FILE *r = fopen("/data/PSXS5/psxs5.ini", "r");
        FILE *w = fopen("/data/PSXS5/.sandbox-test", "w");
        snprintf(sandbox_probe, sizeof(sandbox_probe), "sandboxed read %s, write %s, list %s",
                 r ? "ok" : "no", w ? "ok" : "no", ps5_data_listable() ? "ok" : "no");
        if (r)
            fclose(r);
        if (w)
        {
            fclose(w);
            remove("/data/PSXS5/.sandbox-test");
        }
    }
    if (plat_unlock_disabled())
    {
        snprintf(error, size, "unlock turned off in Settings");
        return ps5_data_listable();
    }
    /* 1. etaHEN's jailbreak-on-demand (answered on 13.60). */
    UnlockResult hen = ps5_unlock_etahen();
    if (hen == UNLOCK_OK || hen == UNLOCK_ALREADY)
        return true;
    /* 2. The boilerplate's Lapy helper through the ELF loader (6.02/12.70). */
    const char *route = "none";
    int status = psxs5_elevate(&route);
    if (status == 0 && ps5_data_listable())
        return true;
    /* 3. Sandboxed: files in /data still open and save, folders can't be listed. */
    snprintf(error, size, "%s; Lapy code %d via %s", ps5_unlock_describe(hen), status,
             route);
    return false;
#else
    (void)error;
    (void)size;
#endif
    return true;
}

/* ---------------------------------------------------------------- input */

/* order[player] = the controller (Controls > Players order) */
static int player_order[PSXS5_MAX_PADS] = {0, 1, 2, 3};

void plat_set_player_order(const int order[PSXS5_MAX_PADS])
{
    memcpy(player_order, order, sizeof(player_order));
}

void plat_player_order(int order[PSXS5_MAX_PADS])
{
    memcpy(order, player_order, sizeof(player_order));
}

static int physical(int port)
{
    return port >= 0 && port < PSXS5_MAX_PADS ? player_order[port] : -1;
}

static void order_pads(PadState pads[PSXS5_MAX_PADS])
{
    PadState raw[PSXS5_MAX_PADS];
    memcpy(raw, pads, sizeof(raw));
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
        pads[i] = raw[player_order[i]];
}

#if defined(__PROSPERO__)
/* The PS5 SDL driver reports DualSense buttons in its own order (see
 * ps5-payload-dev/SDL src/joystick/ps5), but SDL's controller database maps
 * the pad with a PC "PS5 Controller" profile that numbers them differently,
 * so buttons came out wrong (Triangle acted as Circle). Read the raw
 * joystick in the driver's order instead. The driver has no Create button
 * (slot 5); PSXS5 turns a touchpad tap into Select. */
static SDL_Joystick *joys[PSXS5_MAX_PADS];
static int pad_uid[PSXS5_MAX_PADS] = {-1, -1, -1, -1}; /* the PS5 user of each */
static int pad_h[PSXS5_MAX_PADS] = {-1, -1, -1, -1};   /* its scePad handle */
static bool motion_on, touch_on, motion_set[PSXS5_MAX_PADS];

/* scePad, for what the SDL driver doesn't pass on: the motion sensor and the
 * adaptive triggers (layouts as in Sony's pad.h; the read gets room to spare) */
typedef struct
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint16_t padding;
    float quat[4];
    float vel[3];
    float acc[3];
    uint8_t touch[24];
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t reserve[2];
    uint8_t unique_len;
    uint8_t unique[12];
} Ps5PadData;
typedef union
{
    Ps5PadData d;
    uint8_t raw[512];
} Ps5PadBuffer;
typedef struct
{
    uint32_t mode;
    uint8_t padding[4];
    uint8_t data[48];
} Ps5TriggerCommand;
typedef struct
{
    uint8_t mask; /* 1 L2, 2 R2 */
    uint8_t padding[7];
    Ps5TriggerCommand command[2];
} Ps5TriggerParam;
_Static_assert(sizeof(Ps5TriggerParam) == 120, "ScePadTriggerEffectParam is 120 bytes");
int sceUserServiceGetLoginUserIdList(int ids[4]);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, void *data);
int scePadSetMotionSensorState(int handle, bool enable);
int scePadSetTriggerEffect(int handle, const Ps5TriggerParam *param);

static int pad_handle(int i)
{
    if (i < 0 || i >= PSXS5_MAX_PADS || !joys[i] || pad_uid[i] < 0)
        return -1;
    if (pad_h[i] < 0)
        pad_h[i] = scePadGetHandle(pad_uid[i], 0, 0);
    return pad_h[i];
}

static const int ps5_buttons[] = {
    BTN_CROSS, BTN_CIRCLE, BTN_SQUARE, BTN_TRIANGLE, BTN_MENU /* touchpad */, -1 /* none */,
    BTN_START /* Options */, BTN_L3, BTN_R3, BTN_L1, BTN_R1, BTN_UP, BTN_DOWN, BTN_LEFT,
    BTN_RIGHT, BTN_L2, BTN_R2,
};

static void refresh_controllers(void)
{
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
    {
        if (joys[i] && !SDL_JoystickGetAttached(joys[i]))
        {
            SDL_JoystickClose(joys[i]);
            joys[i] = NULL;
            pad_uid[i] = -1;
        }
        pad_h[i] = -1;
        motion_set[i] = false;
    }
    /* the driver lists one controller per signed-in user, in this order */
    int ids[4] = {-1, -1, -1, -1}, user_of[8], users = 0;
    if (sceUserServiceGetLoginUserIdList(ids) == 0)
        for (int k = 0; k < 4; ++k)
            if (ids[k] != -1)
                user_of[users++] = ids[k];
    for (int j = 0; j < SDL_NumJoysticks(); ++j)
    {
        const char *name = SDL_JoystickNameForIndex(j);
        if (name && strstr(name, "Remote"))
            continue; /* "PS5 Remote Control": not a gamepad */
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        bool open = false;
        for (int i = 0; i < PSXS5_MAX_PADS; ++i)
            open |= joys[i] && SDL_JoystickInstanceID(joys[i]) == id;
        for (int i = 0; i < PSXS5_MAX_PADS && !open; ++i)
            if (!joys[i])
            {
                joys[i] = SDL_JoystickOpen(j);
                pad_uid[i] = j < users ? user_of[j] : -1;
                if (joys[i])
                    psxs5_log("pad %d: %s (%d buttons, %d axes)", i + 1, name ? name : "?",
                              SDL_JoystickNumButtons(joys[i]), SDL_JoystickNumAxes(joys[i]));
                open = true;
            }
    }
}

void plat_poll(PadState pads[PSXS5_MAX_PADS], bool *quit)
{
    SDL_Event event;
    bool devices_changed = false;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_JOYDEVICEADDED || event.type == SDL_JOYDEVICEREMOVED)
            devices_changed = true;
    (void)quit;
    static bool first = true;
    if (devices_changed || first)
    {
        refresh_controllers();
        first = false;
    }
    memset(pads, 0, sizeof(PadState) * PSXS5_MAX_PADS);
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
    {
        SDL_Joystick *j = joys[i];
        if (!j)
            continue;
        PadState *p = &pads[i];
        p->connected = true;
        int count = SDL_JoystickNumButtons(j);
        for (int b = 0; b < count && b < (int)(sizeof(ps5_buttons) / sizeof(ps5_buttons[0])); ++b)
            if (ps5_buttons[b] >= 0 && SDL_JoystickGetButton(j, b))
                p->buttons |= BIT(ps5_buttons[b]);
        p->lx = SDL_JoystickGetAxis(j, 0);
        p->ly = SDL_JoystickGetAxis(j, 1);
        p->rx = SDL_JoystickGetAxis(j, 2);
        p->ry = SDL_JoystickGetAxis(j, 3);
        p->l2 = (uint8_t)((SDL_JoystickGetAxis(j, 4) + 32768) >> 8);
        p->r2 = (uint8_t)((SDL_JoystickGetAxis(j, 5) + 32768) >> 8);
        int h = motion_on || touch_on ? pad_handle(i) : -1;
        if (h >= 0)
        {
            if (motion_on && !motion_set[i])
            {
                int err = scePadSetMotionSensorState(h, true);
                psxs5_log("pad %d: motion sensor %s (0x%08x)", i + 1, err ? "failed" : "on", (unsigned)err);
                motion_set[i] = true;
            }
            Ps5PadBuffer b;
            memset(&b, 0, sizeof(b));
            if (scePadReadState(h, &b) == 0)
            {
                memcpy(p->quat, b.d.quat, sizeof(p->quat));
                p->motion = motion_on && (b.d.quat[0] || b.d.quat[1] || b.d.quat[2] || b.d.quat[3]);
                /* touch: a finger count, then points of x, y (16 bits each), finger id */
                p->touching = touch_on && b.d.touch[0] > 0;
                p->touch_x = (uint16_t)(b.d.touch[8] | b.d.touch[9] << 8);
                p->touch_y = (uint16_t)(b.d.touch[10] | b.d.touch[11] << 8);
                /* where the battery is isn't documented: log the extra bytes when they change */
                static uint8_t last[PSXS5_MAX_PADS][32];
                static uint64_t logged_at[PSXS5_MAX_PADS];
                uint64_t now = plat_ticks_us();
                if (memcmp(last[i], b.d.ext, 32) != 0 && now - logged_at[i] > 30000000ull)
                {
                    memcpy(last[i], b.d.ext, 32);
                    logged_at[i] = now;
                    char hex[100];
                    for (int k = 0; k < 32; ++k)
                        snprintf(hex + k * 3, 4, "%02x ", b.d.ext[k]);
                    psxs5_log("pad %d extra: %s", i + 1, hex);
                }
            }
        }
    }
    order_pads(pads);
}

const char *plat_pad_name(int port)
{
    int i = physical(port);
    return i >= 0 && joys[i] ? SDL_JoystickName(joys[i]) : NULL;
}

void plat_pad_motion(bool on)
{
    motion_on = on;
}

void plat_pad_touch(bool on)
{
    touch_on = on;
}

void plat_pad_triggers(int port, PlatTrigger l2, PlatTrigger r2)
{
    int i = physical(port);
    static PlatTrigger last[PSXS5_MAX_PADS][2];
    static int last_h[PSXS5_MAX_PADS] = {-2, -2, -2, -2};
    int h = pad_handle(i);
    if (h < 0)
        return;
    if (last_h[i] == h && !memcmp(&last[i][0], &l2, sizeof(l2)) && !memcmp(&last[i][1], &r2, sizeof(r2)))
        return;
    last_h[i] = h;
    last[i][0] = l2;
    last[i][1] = r2;
    Ps5TriggerParam param;
    memset(&param, 0, sizeof(param));
    param.mask = 3;
    const PlatTrigger *t[2] = {&l2, &r2};
    for (int k = 0; k < 2; ++k)
    {
        param.command[k].mode = t[k]->mode;
        param.command[k].data[0] = t[k]->a;
        param.command[k].data[1] = t[k]->b;
        param.command[k].data[2] = t[k]->c;
        param.command[k].data[3] = t[k]->d;
    }
    int err = scePadSetTriggerEffect(h, &param);
    static bool logged;
    if (err && !logged)
    {
        psxs5_log("pad: trigger effect failed (0x%08x)", (unsigned)err);
        logged = true;
    }
}

int plat_pad_battery(int port)
{
    (void)port;
    return -1;
}

void plat_set_lightbar(int port, uint32_t rgb)
{
    static uint32_t last[PSXS5_MAX_PADS] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    port = physical(port);
    if (port < 0 || port >= PSXS5_MAX_PADS || !joys[port] || last[port] == rgb)
        return;
    last[port] = rgb;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_JoystickSetLED(joys[port], (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb) != 0)
    {
        static bool logged;
        if (!logged)
            psxs5_log("pad: light bar not supported here (%s)", SDL_GetError());
        logged = true;
    }
#endif
}

void plat_rumble(int port, uint16_t strong, uint16_t weak)
{
    port = physical(port);
    if (port < 0 || port >= PSXS5_MAX_PADS || !joys[port])
        return;
    if (rumble_strong[port] == strong && rumble_weak[port] == weak)
        return;
    rumble_strong[port] = strong;
    rumble_weak[port] = weak;
    SDL_JoystickRumble(joys[port], strong, weak, strong || weak ? 2000 : 0);
}
#else

static void refresh_controllers(void)
{
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
    {
        if (controllers[i] && !SDL_GameControllerGetAttached(controllers[i]))
        {
            SDL_GameControllerClose(controllers[i]);
            controllers[i] = NULL;
        }
    }
    for (int j = 0; j < SDL_NumJoysticks(); ++j)
    {
        if (!SDL_IsGameController(j))
            continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        bool open = false;
        for (int i = 0; i < PSXS5_MAX_PADS; ++i)
            if (controllers[i] &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[i])) == id)
                open = true;
        if (open)
            continue;
        for (int i = 0; i < PSXS5_MAX_PADS; ++i)
        {
            if (!controllers[i])
            {
                controllers[i] = SDL_GameControllerOpen(j);
                if (controllers[i])
                    psxs5_log("pad %d: %s", i + 1, SDL_GameControllerName(controllers[i]));
                break;
            }
        }
    }
}

static const struct
{
    SDL_GameControllerButton sdl;
    int bit;
} button_map[] = {
    {SDL_CONTROLLER_BUTTON_A, BTN_CROSS},
    {SDL_CONTROLLER_BUTTON_B, BTN_CIRCLE},
    {SDL_CONTROLLER_BUTTON_X, BTN_SQUARE},
    {SDL_CONTROLLER_BUTTON_Y, BTN_TRIANGLE},
    {SDL_CONTROLLER_BUTTON_BACK, BTN_SELECT},
    {SDL_CONTROLLER_BUTTON_START, BTN_START},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L1},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, BTN_L3},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, BTN_R3},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT},
#if SDL_VERSION_ATLEAST(2, 0, 14)
    {SDL_CONTROLLER_BUTTON_TOUCHPAD, BTN_MENU},
#endif
};

static const struct
{
    SDL_Scancode key;
    int bit;
} key_map[] = {
    {SDL_SCANCODE_Z, BTN_CROSS},      {SDL_SCANCODE_X, BTN_CIRCLE},
    {SDL_SCANCODE_A, BTN_SQUARE},     {SDL_SCANCODE_S, BTN_TRIANGLE},
    {SDL_SCANCODE_RSHIFT, BTN_SELECT}, {SDL_SCANCODE_RETURN, BTN_START},
    {SDL_SCANCODE_Q, BTN_L1},         {SDL_SCANCODE_W, BTN_R1},
    {SDL_SCANCODE_1, BTN_L2},         {SDL_SCANCODE_2, BTN_R2},
    {SDL_SCANCODE_UP, BTN_UP},        {SDL_SCANCODE_DOWN, BTN_DOWN},
    {SDL_SCANCODE_LEFT, BTN_LEFT},    {SDL_SCANCODE_RIGHT, BTN_RIGHT},
    {SDL_SCANCODE_ESCAPE, BTN_MENU},  {SDL_SCANCODE_TAB, BTN_MENU},
};

void plat_poll(PadState pads[PSXS5_MAX_PADS], bool *quit)
{
    SDL_Event event;
    bool devices_changed = false;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT && quit)
            *quit = true;
        if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED)
            devices_changed = true;
    }
    static bool first = true;
    if (devices_changed || first)
    {
        refresh_controllers();
        first = false;
    }

    memset(pads, 0, sizeof(PadState) * PSXS5_MAX_PADS);
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
    {
        SDL_GameController *c = controllers[i];
        if (!c)
            continue;
        PadState *p = &pads[i];
        p->connected = true;
        for (size_t b = 0; b < sizeof(button_map) / sizeof(button_map[0]); ++b)
            if (SDL_GameControllerGetButton(c, button_map[b].sdl))
                p->buttons |= BIT(button_map[b].bit);
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000)
            p->buttons |= BIT(BTN_L2);
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000)
            p->buttons |= BIT(BTN_R2);
        p->lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        p->ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        p->rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        p->ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
        p->l2 = (uint8_t)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        p->r2 = (uint8_t)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    }
    order_pads(pads);

#if defined(PSXS5_PREVIEW)
    preview_step();
    pads[0].connected = true;
    pads[0].buttons = preview_buttons;
    if (preview_quit && quit)
        *quit = true;
#endif
#if !defined(__PROSPERO__)
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    for (size_t k = 0; k < sizeof(key_map) / sizeof(key_map[0]); ++k)
        if (keys[key_map[k].key])
        {
            pads[0].buttons |= BIT(key_map[k].bit);
            pads[0].connected = true;
        }
#else
    (void)key_map;
#endif
}

const char *plat_pad_name(int port)
{
    int i = physical(port);
    return i >= 0 && controllers[i] ? SDL_GameControllerName(controllers[i]) : NULL;
}

void plat_pad_motion(bool on)
{
    (void)on;
}

void plat_pad_touch(bool on)
{
    (void)on;
}

void plat_pad_triggers(int port, PlatTrigger l2, PlatTrigger r2)
{
    (void)port, (void)l2, (void)r2;
}

int plat_pad_battery(int port)
{
    (void)port;
    return -1;
}

void plat_set_lightbar(int port, uint32_t rgb)
{
    port = physical(port);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (port >= 0 && port < PSXS5_MAX_PADS && controllers[port])
        SDL_GameControllerSetLED(controllers[port], (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
#else
    (void)port, (void)rgb;
#endif
}

void plat_rumble(int port, uint16_t strong, uint16_t weak)
{
    port = physical(port);
    if (port < 0 || port >= PSXS5_MAX_PADS || !controllers[port])
        return;
    if (rumble_strong[port] == strong && rumble_weak[port] == weak)
        return;
    rumble_strong[port] = strong;
    rumble_weak[port] = weak;
    SDL_GameControllerRumble(controllers[port], strong, weak, strong || weak ? 2000 : 0);
}
#endif

/* ---------------------------------------------------------------- audio */

bool plat_audio_open(int sample_rate)
{
    if (audio_device && audio_rate == sample_rate)
    {
        SDL_ClearQueuedAudio(audio_device);
        return true;
    }
    plat_audio_close();
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); /* SDL resamples for us */
    if (!audio_device)
    {
        psxs5_log("audio open failed: %s", SDL_GetError());
        return false;
    }
    audio_rate = sample_rate;
    SDL_PauseAudioDevice(audio_device, 0);
    psxs5_log("audio: %d Hz", sample_rate);
    return true;
}

void plat_audio_close(void)
{
    if (audio_device)
        SDL_CloseAudioDevice(audio_device);
    audio_device = 0;
    audio_rate = 0;
}

void plat_audio_push(const int16_t *frames, size_t frame_count)
{
    if (audio_device && frame_count)
        SDL_QueueAudio(audio_device, frames, (Uint32)(frame_count * 4));
}

size_t plat_audio_queued_frames(void)
{
    return audio_device ? SDL_GetQueuedAudioSize(audio_device) / 4 : 0;
}

void plat_audio_clear(void)
{
    if (audio_device)
        SDL_ClearQueuedAudio(audio_device);
}

/* ---------------------------------------------------------------- video */

int plat_width(void)
{
    return out_w;
}

int plat_height(void)
{
    return out_h;
}

void plat_begin_frame(uint32_t clear_argb)
{
    SDL_RenderSetClipRect(renderer, NULL);
    if ((clear_argb >> 24) == 0)
        return; /* the screen draws an opaque backdrop: skip the clear */
    set_draw_color(clear_argb);
    SDL_RenderClear(renderer);
}

/* ---- prescalers (XRGB8888). Scale2x/Scale3x are the AdvanceMAME edge rules:
 * they round off staircase edges without blurring flat areas. */

static void prescale_sharp(const uint32_t *src, int w, int h, size_t pitch_px, uint32_t *dst, int k)
{
    int dw = w * k;
    for (int y = 0; y < h; ++y)
    {
        uint32_t *row = dst + (size_t)y * k * dw;
        const uint32_t *s = src + (size_t)y * pitch_px;
        for (int x = 0; x < w; ++x)
            for (int i = 0; i < k; ++i)
                row[x * k + i] = s[x];
        for (int r = 1; r < k; ++r)
            memcpy(row + (size_t)r * dw, row, (size_t)dw * 4);
    }
}

static void scale2x(const uint32_t *src, int w, int h, size_t pitch_px, uint32_t *dst)
{
    int dw = w * 2;
    for (int y = 0; y < h; ++y)
    {
        const uint32_t *up = src + (size_t)(y > 0 ? y - 1 : y) * pitch_px;
        const uint32_t *row = src + (size_t)y * pitch_px;
        const uint32_t *dn = src + (size_t)(y < h - 1 ? y + 1 : y) * pitch_px;
        uint32_t *o0 = dst + (size_t)y * 2 * dw, *o1 = o0 + dw;
        for (int x = 0; x < w; ++x)
        {
            uint32_t B = up[x], D = row[x > 0 ? x - 1 : x], E = row[x];
            uint32_t F = row[x < w - 1 ? x + 1 : x], H = dn[x];
            bool edge = B != H && D != F;
            o0[x * 2] = edge && D == B ? D : E;
            o0[x * 2 + 1] = edge && B == F ? F : E;
            o1[x * 2] = edge && D == H ? D : E;
            o1[x * 2 + 1] = edge && H == F ? F : E;
        }
    }
}

static void scale3x(const uint32_t *src, int w, int h, size_t pitch_px, uint32_t *dst)
{
    int dw = w * 3;
    for (int y = 0; y < h; ++y)
    {
        const uint32_t *up = src + (size_t)(y > 0 ? y - 1 : y) * pitch_px;
        const uint32_t *row = src + (size_t)y * pitch_px;
        const uint32_t *dn = src + (size_t)(y < h - 1 ? y + 1 : y) * pitch_px;
        uint32_t *o0 = dst + (size_t)y * 3 * dw, *o1 = o0 + dw, *o2 = o1 + dw;
        for (int x = 0; x < w; ++x)
        {
            int l = x > 0 ? x - 1 : x, r = x < w - 1 ? x + 1 : x;
            uint32_t A = up[l], B = up[x], C = up[r], D = row[l], E = row[x], F = row[r];
            uint32_t G = dn[l], H = dn[x], I = dn[r];
            uint32_t *p0 = &o0[x * 3], *p1 = &o1[x * 3], *p2 = &o2[x * 3];
            if (B != H && D != F)
            {
                p0[0] = D == B ? D : E;
                p0[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                p0[2] = B == F ? F : E;
                p1[0] = (D == B && E != G) || (D == H && E != A) ? D : E;
                p1[1] = E;
                p1[2] = (B == F && E != I) || (H == F && E != C) ? F : E;
                p2[0] = D == H ? D : E;
                p2[1] = (D == H && E != I) || (H == F && E != G) ? H : E;
                p2[2] = H == F ? F : E;
            }
            else
                p0[0] = p0[1] = p0[2] = p1[0] = p1[1] = p1[2] = p2[0] = p2[1] = p2[2] = E;
        }
    }
}

static uint32_t *scale_buf[2];
static size_t scale_buf_size[2];

static uint32_t *scratch(int which, size_t pixels)
{
    if (scale_buf_size[which] < pixels)
    {
        SDL_free(scale_buf[which]);
        scale_buf[which] = SDL_malloc(pixels * 4);
        scale_buf_size[which] = scale_buf[which] ? pixels : 0;
    }
    return scale_buf[which];
}

static int game_tex_w, game_tex_h;
static int game_src_w = 320, game_src_h = 240; /* before prescaling, for aspect maths */

static bool ensure_game_texture(int format, int w, int h)
{
    if (game_texture && format == game_texture_format && w <= game_tex_w && h <= game_tex_h)
        return true;
    if (game_texture)
        SDL_DestroyTexture(game_texture);
    int tw = w > GAME_TEX_W ? w : GAME_TEX_W, th = h > GAME_TEX_H ? h : GAME_TEX_H;
    Uint32 fmt = format == 1 ? SDL_PIXELFORMAT_ARGB8888
               : format == 2 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB555;
    game_texture = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_STREAMING, tw, th);
    game_texture_format = game_texture ? format : -1;
    game_tex_w = game_texture ? tw : 0;
    game_tex_h = game_texture ? th : 0;
    return game_texture != NULL;
}

#if defined(__PROSPERO__)
/* PS5: the latest game picture, kept in PSXS5's memory and scaled straight
 * into the canvas by blit.c (SDL's software stretch was the bottleneck). */
static const uint32_t *game_image;
static int game_image_w, game_image_h;
static size_t game_image_pitch;
#endif

static bool game_gpu; /* v2: the picture is the core's Vulkan image */

/* ---------------------------------------------------------------- picture colours */

static float colour_k[3] = {1.0f, 1.0f, 0.0f}; /* brightness, saturation, warmth */
static bool colour_on;
static uint8_t colour_lut[3][256]; /* per channel: brightness and warmth */

void plat_set_colour(int brightness, int colour, int sharpen)
{
    static int last_b = -1, last_c = -1, last_s = -1;
    if (brightness == last_b && colour == last_c && sharpen == last_s)
        return;
    last_b = brightness;
    last_c = colour;
    last_s = sharpen;
    static const float bright[] = {0.85f, 1.0f, 1.12f, 1.25f};
    static const float sat[] = {1.0f, 1.3f, 0.78f, 1.05f, 1.0f, 0.0f};
    static const float warm[] = {0.0f, 0.0f, 0.0f, 0.06f, -0.06f, 0.0f};
    colour_k[0] = bright[brightness >= 0 && brightness < 4 ? brightness : 1];
    colour_k[1] = sat[colour >= 0 && colour < 6 ? colour : 0];
    colour_k[2] = warm[colour >= 0 && colour < 6 ? colour : 0];
    colour_on = colour_k[0] != 1.0f || colour_k[1] != 1.0f || colour_k[2] != 0.0f;
    const float gain[3] = {colour_k[0] * (1.0f + colour_k[2]), colour_k[0], colour_k[0] * (1.0f - colour_k[2])};
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v)
        {
            float x = v * gain[c];
            colour_lut[c][v] = (uint8_t)(x > 255.0f ? 255 : x);
        }
#if defined(__PROSPERO__)
    static const float sharp[] = {0.0f, 0.45f, 1.0f};
    vkp_set_colour(colour_k[0], colour_k[1], colour_k[2], sharp[sharpen >= 0 && sharpen < 3 ? sharpen : 0]);
#endif
}

/* The PCSX-ReARMed picture (XRGB8888) graded on the CPU. */
static void grade_pixels(uint32_t *px, int w, int h, size_t pitch)
{
    if (!colour_on)
        return;
    int s = (int)(colour_k[1] * 256.0f);
    for (int y = 0; y < h; ++y)
    {
        uint32_t *row = px + (size_t)y * pitch;
        for (int x = 0; x < w; ++x)
        {
            uint32_t p = row[x];
            int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
            if (s != 256)
            {
                int l = (r * 77 + g * 150 + b * 29) >> 8;
                r = l + (((r - l) * s) >> 8);
                g = l + (((g - l) * s) >> 8);
                b = l + (((b - l) * s) >> 8);
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
            }
            row[x] = (p & 0xff000000u) | (uint32_t)colour_lut[0][r] << 16 | (uint32_t)colour_lut[1][g] << 8 |
                     colour_lut[2][b];
        }
    }
}

/* ---------------------------------------------------------------- clock */

#if defined(__PROSPERO__)
int sceSystemServiceParamGetInt(int id, int *value);
#endif

void plat_clock(char *out, size_t size)
{
    time_t now = time(NULL);
    bool h24 = true;
#if defined(__PROSPERO__)
    /* the console's time zone (minutes from UTC), summer time and 12/24 h */
    int zone = 0, summer = 0, format = 1;
    sceSystemServiceParamGetInt(4, &zone);   /* TIME_ZONE */
    sceSystemServiceParamGetInt(5, &summer); /* SUMMERTIME */
    if (sceSystemServiceParamGetInt(3, &format) == 0) /* TIME_FORMAT: 0 12 h, 1 24 h */
        h24 = format != 0;
    long long t = (long long)now + (long long)zone * 60 + (summer ? 3600 : 0);
    int minutes = (int)((t / 60) % 1440);
    if (minutes < 0)
        minutes += 1440;
    int hour = minutes / 60, minute = minutes % 60;
#else
    struct tm tm;
    if (!local_time((long long)now, &tm))
    {
        out[0] = '\0';
        return;
    }
    int hour = tm.tm_hour, minute = tm.tm_min;
#endif
    if (h24)
        snprintf(out, size, "%02d:%02d", hour, minute);
    else
        snprintf(out, size, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
}

void plat_upload_game_gpu(int width, int height)
{
    game_gpu = true;
    game_src_w = width;
    game_src_h = height;
}

void plat_upload_game(const void *pixels, int width, int height, size_t pitch, int pixel_format,
                      int upscale, int filter)
{
    if (!pixels || width <= 0 || height <= 0)
        return;
    game_gpu = false;
    game_src_w = width;
    game_src_h = height;
    int k = pixel_format == 1 ? upscale : 1; /* prescalers work on 32-bit frames */
    while (k > 1 && (width * k > 4096 || height * k > 2048))
        --k; /* keep the texture within limits (2x internal frames are already big) */

#if defined(__PROSPERO__)
    if (pixel_format == 1)
    {
        const uint32_t *src = pixels;
        size_t pitch_px = pitch / 4;
        uint32_t *out = scratch(0, (size_t)width * k * height * k);
        if (!out)
            return;
        if (k <= 1)
            for (int y = 0; y < height; ++y) /* the core reuses its buffer: keep a copy */
                memcpy(out + (size_t)y * width, src + (size_t)y * pitch_px, (size_t)width * 4);
        else if (filter == UPSCALE_XBR)
        {
            /* One xBR pass (2x) for any upscale: a second pass on 4x the
             * pixels cost too much; the smooth final scale does the rest.
             * Hi-res frames (menus, interlaced 640x480) don't need it. */
            if (width > 400)
            {
                for (int y = 0; y < height; ++y)
                    memcpy(out + (size_t)y * width, src + (size_t)y * pitch_px, (size_t)width * 4);
                k = 1;
            }
            else
            {
                xbr2x(src, width, height, pitch_px, out);
                k = 2;
            }
        }
        else if (filter == UPSCALE_SMOOTH_PIXELS && k == 2)
            scale2x(src, width, height, pitch_px, out);
        else if (filter == UPSCALE_SMOOTH_PIXELS && k == 3)
            scale3x(src, width, height, pitch_px, out);
        else if (filter == UPSCALE_SMOOTH_PIXELS && k == 4)
        {
            uint32_t *mid = scratch(1, (size_t)width * 2 * height * 2);
            if (!mid)
                return;
            scale2x(src, width, height, pitch_px, mid);
            scale2x(mid, width * 2, height * 2, (size_t)width * 2, out);
        }
        else
            prescale_sharp(src, width, height, pitch_px, out, k);
        grade_pixels(out, width * (k > 1 ? k : 1), height * (k > 1 ? k : 1), (size_t)width * (k > 1 ? k : 1));
        game_image = out;
        game_image_w = width * (k > 1 ? k : 1);
        game_image_h = height * (k > 1 ? k : 1);
        game_image_pitch = (size_t)game_image_w;
        return;
    }
#endif

    if (k <= 1)
    {
        if (!ensure_game_texture(pixel_format, width, height))
            return;
        SDL_Rect rect = {0, 0, width, height};
        SDL_UpdateTexture(game_texture, &rect, pixels, (int)pitch);
        game_w = width;
        game_h = height;
        return;
    }

    const uint32_t *src = pixels;
    size_t pitch_px = pitch / 4;
    uint32_t *out = scratch(0, (size_t)width * k * height * k);
    if (!out)
        return;
    if (filter == UPSCALE_XBR)
    {
        xbr2x(src, width, height, pitch_px, out); /* one pass; the final scale does the rest */
        k = 2;
    }
    else if (filter == UPSCALE_SMOOTH_PIXELS && k == 2)
        scale2x(src, width, height, pitch_px, out);
    else if (filter == UPSCALE_SMOOTH_PIXELS && k == 3)
        scale3x(src, width, height, pitch_px, out);
    else if (filter == UPSCALE_SMOOTH_PIXELS && k == 4)
    {
        uint32_t *mid = scratch(1, (size_t)width * 2 * height * 2);
        if (!mid)
            return;
        scale2x(src, width, height, pitch_px, mid);
        scale2x(mid, width * 2, height * 2, (size_t)width * 2, out);
    }
    else
        prescale_sharp(src, width, height, pitch_px, out, k);

    if (!ensure_game_texture(pixel_format, width * k, height * k))
        return;
    SDL_Rect rect = {0, 0, width * k, height * k};
    SDL_UpdateTexture(game_texture, &rect, out, width * k * 4);
    game_w = width * k;
    game_h = height * k;
}

static int game_rect[4];

void plat_game_rect(int *x, int *y, int *w, int *h)
{
    *x = game_rect[0];
    *y = game_rect[1];
    *w = game_rect[2];
    *h = game_rect[3];
}

void plat_draw_game(const Settings *settings, float display_aspect, uint8_t dim)
{
#if defined(__PROSPERO__)
    if (!game_image && !game_texture && !(game_gpu && vkp_game_image_ready()))
        return;
#else
    if (!game_texture)
        return;
#endif
    /* Scanlines the game actually has: interlaced (480) and 2x-internal frames
     * show at the same size as 240-line ones. */
    int lines = game_src_h;
    while (lines > 288)
        lines /= 2;
    /* Crop black edges: the share of the picture's height cut at the top and
     * at the bottom (8 or 16 of the PS1's 240 lines) */
    const float crop = (settings->crop_edges > 0 && settings->crop_edges < 3 ? settings->crop_edges * 8 : 0) / 240.0f;
    int columns = game_src_w; /* the same for the width (internal resolution) */
    while (columns > 768)
        columns /= 2;

    float aspect;
    switch (settings->aspect)
    {
    case ASPECT_4_3: aspect = 4.0f / 3.0f; break;
    case ASPECT_16_9: aspect = 16.0f / 9.0f; break;
    case ASPECT_16_10: aspect = 16.0f / 10.0f; break;
    case ASPECT_PIXEL:
        aspect = (float)columns / (float)lines;
        break;
    case ASPECT_STRETCH: aspect = (float)out_w / out_h; break;
    default: aspect = display_aspect > 0.0f ? display_aspect : 4.0f / 3.0f; break;
    }

    int dh = out_h, dw = (int)(out_h * aspect + 0.5f);
    if (dw > out_w)
    {
        dw = out_w;
        dh = (int)(out_w / aspect + 0.5f);
    }
    if (settings->integer_scale)
    {
        int k = dh / (lines > 0 ? lines : 240);
        if (k < 1)
            k = 1;
        dh = lines * k;
        dw = settings->aspect == ASPECT_STRETCH ? out_w : (int)(dh * aspect + 0.5f);
        if (dw > out_w)
            dw = out_w;
    }

    if (settings->border == 2 && settings->aspect != ASPECT_STRETCH)
    {
        /* TV frame: leave room for the TV around the picture */
        dw = dw * 86 / 100;
        dh = dh * 86 / 100;
    }
    game_rect[0] = (out_w - dw) / 2;
    game_rect[1] = (out_h - dh) / 2;
    game_rect[2] = dw;
    game_rect[3] = dh;
    static const uint8_t scan_strength[] = {0, 110, 200};
    uint8_t scan = scan_strength[settings->crt % 3];
#if defined(__PROSPERO__)
    if (game_gpu && vkp_game_image_ready())
    {
        /* a hole in the canvas where the GPU draws the picture; its alpha
         * darkens the picture for the menus (premultiplied black) */
        SDL_Rect hole = {(out_w - dw) / 2, (out_h - dh) / 2, dw, dh};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        if (scan && lines > 0)
        {
            /* CRT scanlines: each row's darkness as blit.c computes it */
            for (int y = 0; y < dh; ++y)
            {
                int64_t pos = ((int64_t)y * 2 + 1) * lines * 128 / dh;
                int phase = (int)(pos & 255) - 128;
                uint32_t edge = (uint32_t)(phase * phase) >> 6;
                uint32_t keep = (uint32_t)dim * (256 - ((edge * scan) >> 8)) >> 8;
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(255 - (keep > 255 ? 255 : keep)));
                SDL_Rect row = {hole.x, hole.y + y, hole.w, 1};
                SDL_RenderFillRect(renderer, &row);
            }
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(255 - dim));
            SDL_RenderFillRect(renderer, &hole);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        /* supersampling: a picture bigger than its place on screen, averaged down */
        int shader = settings->shader;
        if (!shader && settings->supersampling && (game_src_w > dw || game_src_h > dh))
            shader = 3;
        vkp_show_game((float)hole.x, (float)hole.y, (float)hole.w, (float)hole.h, crop, shader, game_src_w,
                                      game_src_h, lines);
        return;
    }
    if (game_image)
    {
        /* run SDL's queued drawing (the clear) first, then write the picture */
        SDL_RenderFlush(renderer);
        int cut = (int)(game_image_h * crop);
        BlitJob job = {game_image + (size_t)cut * game_image_pitch, game_image_w, game_image_h - 2 * cut,
                       game_image_pitch,
                       (uint32_t *)canvas->pixels, (size_t)canvas->pitch / 4,
                       (out_w - dw) / 2, (out_h - dh) / 2, dw, dh, settings->smooth, dim,
                       scan, lines};
        blit_scaled(&job);
        return;
    }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(game_texture, settings->smooth ? SDL_ScaleModeLinear
                                                           : SDL_ScaleModeNearest);
#endif
    SDL_SetTextureColorMod(game_texture, dim, dim, dim);
    SDL_SetTextureBlendMode(game_texture, SDL_BLENDMODE_NONE); /* opaque: no per-pixel blend */
    int cut = (int)(game_h * crop);
    SDL_Rect src = {0, cut, game_w, game_h - 2 * cut};
    SDL_Rect dst = {(out_w - dw) / 2, (out_h - dh) / 2, dw, dh};
    SDL_RenderCopy(renderer, game_texture, &src, &dst);
    if (scan && lines > 0)
    {
        /* desktop: dark bands between the game's lines */
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, scan / 2);
        float step = (float)dh / lines;
        for (int l = 0; l < lines; ++l)
        {
            SDL_Rect r = {dst.x, dst.y + (int)((l + 0.62f) * step), dw, (int)(step * 0.38f) + 1};
            SDL_RenderFillRect(renderer, &r);
        }
    }
}

void plat_fill_rect(int x, int y, int w, int h, uint32_t argb)
{
    SDL_Rect r = {x, y, w, h};
    set_draw_color(argb);
    SDL_RenderFillRect(renderer, &r);
}

#if defined(__PROSPERO__)
/* The shelf's background: SDL's software renderer took 18 ms a frame to draw
 * a tinted full-screen texture. Here: a lookup table per channel, rows split
 * over the blit threads, written straight into the canvas. */
typedef struct
{
    const uint8_t *grey;
    int w, h;
    uint32_t lut[256];
} BackdropJob;

static void backdrop_rows(void *ctx, int begin, int end)
{
    const BackdropJob *j = ctx;
    uint32_t *dst = canvas->pixels;
    size_t pitch = (size_t)canvas->pitch / 4;
    for (int y = begin; y < end; ++y)
    {
        const uint8_t *src = j->grey + (size_t)(y * j->h / out_h) * j->w;
        uint32_t *row = dst + (size_t)y * pitch;
        if (j->w == out_w)
            for (int x = 0; x < out_w; ++x)
                row[x] = j->lut[src[x]];
        else
            for (int x = 0; x < out_w; ++x)
                row[x] = j->lut[src[x * j->w / out_w]];
    }
}
#endif

void plat_draw_backdrop(const uint8_t *grey, int w, int h, uint32_t tint)
{
#if defined(__PROSPERO__)
    static BackdropJob job;
    job.grey = grey;
    job.w = w;
    job.h = h;
    uint32_t r = (tint >> 16) & 0xff, g = (tint >> 8) & 0xff, b = tint & 0xff;
    for (uint32_t v = 0; v < 256; ++v) /* canvas bytes: R, G, B, A */
        job.lut[v] = 0xff000000u | (b * v / 255) << 16 | (g * v / 255) << 8 | (r * v / 255);
    SDL_RenderFlush(renderer); /* anything queued goes under, as drawn so far */
    blit_parallel(backdrop_rows, &job, out_h);
#else
    /* desktop: a texture, made once from the grey picture */
    static PlatTexture *texture;
    if (!texture)
    {
        uint8_t *px = malloc((size_t)w * h * 4);
        if (!px)
            return;
        for (size_t i = 0; i < (size_t)w * h; ++i)
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = grey[i], px[i * 4 + 3] = 255;
        texture = plat_texture_create(px, w, h, true);
        free(px);
    }
    if (texture)
        plat_draw_texture(texture, 0, 0, (float)plat_width(), (float)plat_height(), tint, false);
#endif
}

#define PROFILE_SLOTS 8
static struct
{
    const char *name[PROFILE_SLOTS];
    uint64_t us[PROFILE_SLOTS];
    int count, frames;
    uint64_t last;
} prof;

void plat_profile(const char *name)
{
    SDL_RenderFlush(renderer); /* SDL queues drawing: run it, to time it */
    uint64_t now = plat_ticks_us();
    if (prof.last)
    {
        int i = 0;
        while (i < prof.count && prof.name[i] != name)
            ++i;
        if (i == prof.count && prof.count < PROFILE_SLOTS)
            prof.name[prof.count++] = name;
        if (i < PROFILE_SLOTS)
            prof.us[i] += now - prof.last;
    }
    prof.last = now;
}

static void profile_frame_end(void)
{
    if (!prof.count)
        return;
    prof.last = 0;
    if (++prof.frames < 120)
        return;
    char line[256];
    int w = snprintf(line, sizeof(line), "ui profile (ms/frame):");
    for (int i = 0; i < prof.count && w < (int)sizeof(line) - 24; ++i)
        w += snprintf(line + w, sizeof(line) - (size_t)w, " %s %.1f", prof.name[i], prof.us[i] / 120 / 1000.0);
    psxs5_log("%s", line);
    memset(&prof, 0, sizeof(prof));
}

void plat_end_frame(void)
{
    profile_frame_end();
    SDL_RenderPresent(renderer);
#if defined(PSXS5_PREVIEW)
    preview_clock += 16667;
#endif
#if defined(__PROSPERO__)
    static uint64_t frame_start, draw_us, present_us;
    static int frames;
    uint64_t drawn = plat_ticks_us();
    if (use_vulkan)
        vkp_present(canvas->pixels, (size_t)canvas->pitch); /* waits for vblank */
    else
        ps5_video_present(canvas->pixels, (size_t)canvas->pitch);
    uint64_t shown = plat_ticks_us();
    if (frame_start)
    {
        draw_us += drawn - frame_start;
        present_us += shown - drawn;
        if (++frames == 120)
        {
            psxs5_log("ui: draw %.1f ms, present %.1f ms per frame (avg of 120)",
                      draw_us / 120 / 1000.0, present_us / 120 / 1000.0);
            frames = 0;
            draw_us = present_us = 0;
        }
    }
    frame_start = shown;
#endif
}

/* ---------------------------------------------------------------- textures and meshes */

struct PlatTexture
{
    SDL_Texture *sdl;
    int width, height;
};

PlatTexture *plat_texture_create(const uint8_t *rgba, int width, int height, bool smooth)
{
    PlatTexture *t = SDL_calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->sdl = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                               width, height); /* ABGR8888 = R,G,B,A bytes in memory */
    if (!t->sdl)
    {
        psxs5_log("texture %dx%d failed: %s", width, height, SDL_GetError());
        SDL_free(t);
        return NULL;
    }
    SDL_UpdateTexture(t->sdl, NULL, rgba, width * 4);
    SDL_SetTextureBlendMode(t->sdl, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(t->sdl, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#else
    (void)smooth;
#endif
    t->width = width;
    t->height = height;
    return t;
}

void plat_texture_free(PlatTexture *t)
{
    if (!t)
        return;
    SDL_DestroyTexture(t->sdl);
    SDL_free(t);
}

void plat_texture_size(const PlatTexture *t, int *width, int *height)
{
    *width = t ? t->width : 0;
    *height = t ? t->height : 0;
}

void plat_draw_mesh(PlatTexture *texture, const PlatVertex *v, int count, const int *indices,
                    int index_count)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    enum { BATCH = 1536 }; /* multiple of 3 */
    static SDL_Vertex out[BATCH];
    if (indices && count > BATCH)
        return; /* indexed meshes are small by construction */
    for (int done = 0; done < count;)
    {
        int n = count - done < BATCH ? count - done : BATCH;
        for (int i = 0; i < n; ++i)
        {
            const PlatVertex *p = &v[done + i];
            out[i].position.x = p->x;
            out[i].position.y = p->y;
            out[i].tex_coord.x = p->u;
            out[i].tex_coord.y = p->v;
            out[i].color.r = (p->argb >> 16) & 0xff;
            out[i].color.g = (p->argb >> 8) & 0xff;
            out[i].color.b = p->argb & 0xff;
            out[i].color.a = p->argb >> 24;
        }
        SDL_RenderGeometry(renderer, texture ? texture->sdl : NULL, out, n, indices,
                           indices ? index_count : 0);
        done += n;
    }
#else
    (void)texture; (void)v; (void)count; (void)indices; (void)index_count;
#endif
}

void plat_draw_texture(PlatTexture *texture, float x, float y, float w, float h, uint32_t tint,
                       bool blend)
{
    if (!texture)
        return;
    SDL_SetTextureBlendMode(texture->sdl, blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetTextureColorMod(texture->sdl, (tint >> 16) & 0xff, (tint >> 8) & 0xff, tint & 0xff);
    SDL_SetTextureAlphaMod(texture->sdl, tint >> 24);
    SDL_Rect dst = {(int)(x + 0.5f), (int)(y + 0.5f), (int)(w + 0.5f), (int)(h + 0.5f)};
    SDL_RenderCopy(renderer, texture->sdl, NULL, &dst);
    SDL_SetTextureBlendMode(texture->sdl, SDL_BLENDMODE_BLEND);
}

void plat_draw_texture_region(PlatTexture *texture, int sx, int sy, int sw, int sh, float x,
                              float y, float w, float h, uint32_t tint)
{
    if (!texture || (tint >> 24) == 0)
        return;
    SDL_SetTextureColorMod(texture->sdl, (tint >> 16) & 0xff, (tint >> 8) & 0xff, tint & 0xff);
    SDL_SetTextureAlphaMod(texture->sdl, tint >> 24);
    SDL_Rect src = {sx, sy, sw, sh};
    int x0 = (int)(x + 0.5f), y0 = (int)(y + 0.5f);
    SDL_Rect dst = {x0, y0, (int)(x + w + 0.5f) - x0, (int)(y + h + 0.5f) - y0};
    if (dst.w > 0 && dst.h > 0)
        SDL_RenderCopy(renderer, texture->sdl, &src, &dst);
}

void plat_fill_rectf(float x, float y, float w, float h, uint32_t argb)
{
    if ((argb >> 24) == 0)
        return;
    int x0 = (int)(x + 0.5f), y0 = (int)(y + 0.5f);
    SDL_Rect r = {x0, y0, (int)(x + w + 0.5f) - x0, (int)(y + h + 0.5f) - y0};
    if (r.w <= 0 || r.h <= 0)
        return;
    SDL_SetRenderDrawBlendMode(renderer, (argb >> 24) == 0xff ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    set_draw_color(argb);
    SDL_RenderFillRect(renderer, &r);
}

void plat_set_clip(int x, int y, int w, int h)
{
    SDL_Rect r = {x, y, w, h};
    SDL_RenderSetClipRect(renderer, w > 0 && h > 0 ? &r : NULL);
}

void plat_asset_path(char *out, size_t size, const char *relative)
{
#if defined(__PROSPERO__)
    /* /app0 exists only inside the sandbox. Once the HEN frees PSXS5 its
     * file system is the console's real one, where the title is mounted at
     * /system_ex/app/<id> (ShadowMountPlus) and stored in /data/homebrew/<id>. */
    static const char *const bases[] = {"/app0/assets", "/system_ex/app/PPSA97510/assets",
                                        "/data/homebrew/PPSA97510/assets"};
    static int chosen = -1;
    if (chosen < 0)
    {
        chosen = 0;
        for (int i = 0; i < 3; ++i)
            if (path_is_dir(bases[i]))
            {
                chosen = i;
                break;
            }
        psxs5_log("assets: %s", bases[chosen]);
    }
    path_join(out, size, bases[chosen], relative);
#else
    const char *base = SDL_getenv("PSXS5_ASSETS");
    path_join(out, size, base && *base ? base : "assets", relative);
#endif
}

uint64_t plat_ticks_us(void)
{
#if defined(PSXS5_PREVIEW)
    return 1000000ull + preview_clock;
#endif
    return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency();
}

void plat_sleep_us(uint32_t us)
{
    SDL_Delay(us / 1000);
}

void plat_notify(const char *message)
{
    psxs5_log("notify: %s", message);
#if defined(__PROSPERO__)
    static struct
    {
        uint8_t reserved[45];
        char message[3075];
    } request;
    memset(&request, 0, sizeof(request));
    str_copy(request.message, sizeof(request.message), message);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#endif
}
