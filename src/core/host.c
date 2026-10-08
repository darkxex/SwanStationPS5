/*
 * PSXS5 - libretro host for the statically linked cores.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cores are linked into PSXS5 as static archives: PCSX-ReARMed
 * (libpcsx_rearmed.a, plain retro_*) and, in v2 builds, Beetle PSX HW
 * (libbeetle_psx.a, its retro_* renamed beetle_retro_* by
 * tools/build-beetle.sh). A table of functions picks one per game. Only the
 * environment callbacks the cores rely on are implemented; everything else
 * answers "unsupported" as libretro allows.
 */
#include "host.h"

#include "../gamedb.h"

#include "libretro.h"
#include "../platform/platform.h"
#if defined(PSXS5_VULKAN)
#include "../platform/vk/vk_present.h"
#include "../platform/vk/vk_present_hw.h" /* before libretro_vulkan.h: no prototypes */
#include "libretro_vulkan.h"
#endif
#include "../platform/ps5_crash.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPTIONS 200

/* ---------------------------------------------------------------- the cores */

typedef struct
{
    const char *name;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*run)(void);
    void (*reset)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    void (*cheat_reset)(void);
    void (*cheat_set)(unsigned, bool, const char *);
} CoreApi;

#define CORE_API(label, p)                                                                         \
    {                                                                                              \
        label, p##retro_set_environment, p##retro_set_video_refresh, p##retro_set_audio_sample,  \
            p##retro_set_audio_sample_batch, p##retro_set_input_poll, p##retro_set_input_state,  \
            p##retro_init, p##retro_deinit, p##retro_load_game, p##retro_unload_game,            \
            p##retro_get_system_av_info, p##retro_set_controller_port_device, p##retro_run,      \
            p##retro_reset, p##retro_serialize_size, p##retro_serialize, p##retro_unserialize,   \
            p##retro_get_memory_data, p##retro_get_memory_size, p##retro_cheat_reset,           \
            p##retro_cheat_set                                                                  \
    }

static const CoreApi PCSX = CORE_API("PCSX-ReARMed", );

#if defined(PSXS5_VULKAN)
#if defined(PSXS5_BEETLE)
void beetle_retro_set_environment(retro_environment_t);
void beetle_retro_set_video_refresh(retro_video_refresh_t);
void beetle_retro_set_audio_sample(retro_audio_sample_t);
void beetle_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void beetle_retro_set_input_poll(retro_input_poll_t);
void beetle_retro_set_input_state(retro_input_state_t);
void beetle_retro_init(void);
void beetle_retro_deinit(void);
bool beetle_retro_load_game(const struct retro_game_info *);
void beetle_retro_unload_game(void);
void beetle_retro_get_system_av_info(struct retro_system_av_info *);
void beetle_retro_set_controller_port_device(unsigned, unsigned);
void beetle_retro_run(void);
void beetle_retro_reset(void);
size_t beetle_retro_serialize_size(void);
bool beetle_retro_serialize(void *, size_t);
bool beetle_retro_unserialize(const void *, size_t);
void *beetle_retro_get_memory_data(unsigned);
size_t beetle_retro_get_memory_size(unsigned);
void beetle_retro_cheat_reset(void);
void beetle_retro_cheat_set(unsigned, bool, const char *);
static const CoreApi BEETLE = CORE_API("Beetle PSX HW", beetle_);
#else
/* Beetle isn't built (Makefile: APP_BEETLE=1 brings it back): an empty entry the code never selects. */
static const CoreApi BEETLE;
#endif
void swanstation_retro_set_environment(retro_environment_t);
void swanstation_retro_set_video_refresh(retro_video_refresh_t);
void swanstation_retro_set_audio_sample(retro_audio_sample_t);
void swanstation_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void swanstation_retro_set_input_poll(retro_input_poll_t);
void swanstation_retro_set_input_state(retro_input_state_t);
void swanstation_retro_init(void);
void swanstation_retro_deinit(void);
bool swanstation_retro_load_game(const struct retro_game_info *);
void swanstation_retro_unload_game(void);
void swanstation_retro_get_system_av_info(struct retro_system_av_info *);
void swanstation_retro_set_controller_port_device(unsigned, unsigned);
void swanstation_retro_run(void);
void swanstation_retro_reset(void);
size_t swanstation_retro_serialize_size(void);
bool swanstation_retro_serialize(void *, size_t);
bool swanstation_retro_unserialize(const void *, size_t);
void *swanstation_retro_get_memory_data(unsigned);
size_t swanstation_retro_get_memory_size(unsigned);
void swanstation_retro_cheat_reset(void);
void swanstation_retro_cheat_set(unsigned, bool, const char *);
static const CoreApi SWANSTATION = CORE_API("SwanStation", swanstation_);
#endif

static const CoreApi *core = &PCSX;

typedef struct
{
    char key[64];
    char value[64];
} Option;

static Option options[MAX_OPTIONS];
static int option_count;
static bool options_dirty;

static const Paths *host_paths;
/* The game being loaded, for the HD texture pack check. */
static char loading_path[PSXS5_PATH_MAX];
extern const Paths *app_paths(void);
static PadState pad_state[PSXS5_MAX_PADS];
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_system_av_info av_info;
static struct retro_disk_control_ext_callback disk;
static bool disk_available;
static bool loaded;
static size_t state_size; /* host_state_size(), measured once per game */
static unsigned game_fixes; /* GDB_* fixes for the next game (gamedb.h) */
static int lid_open_frames;   /* frames left before the lid closes after a disc change */
static bool speculative;      /* run-ahead's look-ahead frames: run, drawn, not heard or felt */

static char patches_dir[PSXS5_PATH_MAX];
static bool multitap;

void psxs5_set_patches_dir(const char *dir); /* core: tools/patches/pcsx_rearmed-patchesdir.patch */

static struct retro_memory_descriptor memory_descriptors[32];
static struct retro_memory_map memory_map;

static const void *frame_data;
static unsigned frame_w, frame_h;
static size_t frame_pitch;
static bool frame_fresh;

/* ---------------------------------------------------------------- options */

static Option *find_option(const char *key)
{
    for (int i = 0; i < option_count; ++i)
        if (strcmp(options[i].key, key) == 0)
            return &options[i];
    return NULL;
}

static void set_option(const char *key, const char *value)
{
    Option *opt = find_option(key);
    if (!opt)
    {
        if (option_count >= MAX_OPTIONS)
            return;
        opt = &options[option_count++];
        str_copy(opt->key, sizeof(opt->key), key);
    }
    if (strcmp(opt->value, value) != 0)
    {
        str_copy(opt->value, sizeof(opt->value), value);
        options_dirty = true;
    }
}

/* Legacy variables look like "Description; default|other|...". */
static void register_variables(const struct retro_variable *vars)
{
    for (; vars && vars->key; ++vars)
    {
        if (find_option(vars->key))
            continue; /* keep PSXS5's override */
        const char *semi = strchr(vars->value, ';');
        const char *start = semi ? semi + 1 : vars->value;
        while (*start == ' ')
            ++start;
        char value[64];
        size_t n = strcspn(start, "|");
        if (n >= sizeof(value))
            n = sizeof(value) - 1;
        memcpy(value, start, n);
        value[n] = '\0';
        set_option(vars->key, value);
    }
}

#if defined(PSXS5_VULKAN)
/* Beetle reads a pack from <game folder>/<game file name>-texture-replacements/
 * (by texture hash, no folder listing). Texture tracking costs time and is the
 * risky part, so it's on only when that folder is there. */
static bool texture_pack_present(void)
{
    char dir[PSXS5_PATH_MAX], name[256];
    str_copy(dir, sizeof(dir), loading_path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    str_copy(name, sizeof(name), slash + 1);
    *slash = '\0';
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = '\0';
    char pack[PSXS5_PATH_MAX];
    snprintf(pack, sizeof(pack), "%s/%s-texture-replacements", dir, name);
    bool here = path_is_dir(pack);
    if (here)
        psxs5_log("host: HD texture pack %s", pack);
    return here;
}

static void apply_beetle_options(const Settings *s)
{
    static const char *const regions[] = {"auto", "ntsc-u", "pal"};
    static const char *const scales[] = {"1x(native)", "2x", "4x", "8x", "16x"};
    int level = s->internal_res >= 1 && s->internal_res <= 5 ? s->internal_res : 1;
    if (game_fixes & GDB_NO_UPSCALING)
        level = 1; /* the game breaks above native (DuckStation's database) */
    bool gpu = vkp_describe()[0] != '\0'; /* the screen runs through Vulkan */
    if (!gpu && level > 2)
        level = 2; /* the software renderer: 4x and up would not fit in memory */
    set_option("beetle_psx_hw_renderer", gpu ? "hardware_vk" : "software");
    set_option("beetle_psx_hw_internal_resolution", scales[level - 1]);
    set_option("beetle_psx_hw_region", regions[s->region % REGION_COUNT]);
    /* true colour: 32-bit, so no dithering */
    set_option("beetle_psx_hw_depth", s->true_colour ? "32bpp" : "16bpp(native)");
    set_option("beetle_psx_hw_dither_mode", s->dithering && !s->true_colour ? "1x(native)" : "disabled");
    set_option("beetle_psx_hw_mdec_yuv", s->fmv_smooth ? "enabled" : "disabled");
    /* the software renderer's copy, kept for screen read-backs (FF7's battle swirl):
     * exact, but a stall when they happen; off, the GPU does those effects */
    set_option("beetle_psx_hw_renderer_software_fb", s->fast_effects ? "disabled" : "enabled");
    set_option("beetle_psx_hw_negcon_response", "linear");
    set_option("beetle_psx_hw_negcon_deadzone", "0%");
    /* read as it plays: "precache" loads every disc of a game into memory,
     * and two discs already pass PSXS5's 1 GB */
    set_option("beetle_psx_hw_cd_access_method", "async");
    set_option("beetle_psx_hw_cd_fastload", s->cd_fast && !(game_fixes & GDB_NO_CD_SPEEDUP) ? "4x" : "2x(native)");
    set_option("beetle_psx_hw_skip_bios", s->boot_intro ? "disabled" : "enabled");
    bool pgxp = s->pgxp && !(game_fixes & GDB_NO_PGXP);
    set_option("beetle_psx_hw_pgxp_mode", !pgxp ? "disabled" : (game_fixes & GDB_PGXP_CPU) ? "memory + CPU" : "memory only");
    set_option("beetle_psx_hw_pgxp_texture", pgxp ? "enabled" : "disabled");
    set_option("beetle_psx_hw_widescreen_hack", s->widescreen ? "enabled" : "disabled");
    set_option("beetle_psx_hw_widescreen_hack_aspect_ratio", "16:9");
    /* analog mode from the start for games known to use the sticks; the
     * L1+L2+R1+R2+START+SELECT combination still switches it, as the ANALOG button did */
    set_option("beetle_psx_hw_analog_toggle", (game_fixes & GDB_ANALOG) ? "enabled-analog" : "enabled");
    /* card 0 through SAVE_RAM: PSXS5 keeps it in PCSX-ReARMed's file */
    set_option("beetle_psx_hw_use_mednafen_memcard0_method", "libretro");
    /* Vulkan: never. Beetle repeats a frame there whenever the game didn't
     * switch display buffers, so screens drawn straight into the shown buffer
     * (the white Sony screen, FF7's battle swirl) never reached the screen, and
     * the "repeated" image was one of Beetle's recycled ones (old pictures) */
    set_option("beetle_psx_hw_frame_duping", gpu ? "disabled" : "enabled");
    bool pack = gpu && s->hd_textures && texture_pack_present();
    set_option("beetle_psx_hw_track_textures", pack ? "enabled" : "disabled");
    set_option("beetle_psx_hw_replace_textures", pack ? "enabled" : "disabled");
    set_option("beetle_psx_hw_dump_textures", "disabled");
    set_option("beetle_psx_hw_texture_directory", "content");
    set_option("beetle_psx_hw_hd_caching_method", "lazy");
    set_option("beetle_psx_hw_hd_cache_vram_budget", "2048");
    set_option("beetle_psx_hw_hd_cache_ram_budget", "256"); /* the heap is 1 GB */
    /* fewer slowdowns: a faster CPU, and the GTE (the 3D maths) at one cycle */
    static const char *const cpu[] = {"100%", "150%", "200%"};
    int oc = s->overclock >= 0 && s->overclock <= 2 ? s->overclock : 0;
    set_option("beetle_psx_hw_cpu_freq_scale", cpu[oc]);
    set_option("beetle_psx_hw_gte_overclock", oc ? "enabled" : "disabled");
    set_option("beetle_psx_hw_gpu_overclock", oc == 2 ? "2x" : "1x(native)");
    set_option("beetle_psx_hw_gun_cursor", "off"); /* PSXS5 draws its own */
    /* the picture: anti-aliasing, texture filtering, supersampling, deinterlacing, PAL at 60 Hz */
    static const char *const msaa[] = {"1x", "2x", "4x", "8x", "16x"};
    static const char *const filters[] = {"nearest", "bilinear", "xBR", "SABR", "JINC2", "3-point"};
    int m = s->msaa >= 0 && s->msaa <= 4 && !(game_fixes & GDB_NO_UPSCALING) ? s->msaa : 0;
    set_option("beetle_psx_hw_msaa", msaa[m]);
    int tf = s->texture_filter >= 0 && s->texture_filter <= 5 && !(game_fixes & GDB_NO_TEXTURE_FILTER) ? s->texture_filter : 0;
    set_option("beetle_psx_hw_filter", filters[tf]);
    bool sprites = s->filter_2d && !(game_fixes & GDB_NO_SPRITE_FILTER);
    set_option("beetle_psx_hw_filter_exclude_sprite", sprites ? "disabled" : "all");
    set_option("beetle_psx_hw_filter_exclude_2d_polygon", s->filter_2d ? "disabled" : "all");
    set_option("beetle_psx_hw_super_sampling", "disabled"); /* it made moving scenes flicker */
    static const char *const deint[] = {"weave", "bob", "fastmad"};
    int d = s->deinterlace >= 0 && s->deinterlace <= 2 ? s->deinterlace : 0;
    if (!d && (game_fixes & GDB_DEINTERLACE))
        d = 1; /* the game needs it */
    set_option("beetle_psx_hw_deinterlacer", deint[d]);
    set_option("beetle_psx_hw_pal_video_timing_override", s->pal60 ? "enabled" : "disabled");
}
#endif

#if defined(PSXS5_VULKAN)
/* SwanStation (DuckStation): the Vulkan renderer, the recompiler with LUT
 * fastmem (MMap does not survive a title's sandbox). */
static void apply_swanstation_options(const Settings *s)
{
    static const char *const regions[] = {"Auto", "NTSC-U", "PAL"};
    static const char *const scales[] = {"1", "2", "4", "8", "16"};
    int level = s->internal_res >= 1 && s->internal_res <= 5 ? s->internal_res : 1;
    if (game_fixes & GDB_NO_UPSCALING)
        level = 1;
    bool pgxp = s->pgxp && !(game_fixes & GDB_NO_PGXP);
    set_option("swanstation_GPU_Renderer", "Vulkan");
    set_option("swanstation_GPU_ResolutionScale", scales[level - 1]);
    set_option("swanstation_Console_Region", regions[s->region % REGION_COUNT]);
    set_option("swanstation_GPU_TrueColor", s->true_colour ? "true" : "false");
    set_option("swanstation_GPU_PGXPEnable", pgxp ? "true" : "false");
    set_option("swanstation_BIOS_PatchFastBoot", s->boot_intro ? "false" : "true");
    set_option("swanstation_CDROM_ReadSpeedup", s->cd_fast && !(game_fixes & GDB_NO_CD_SPEEDUP) ? "4" : "1");
    set_option("swanstation_CPU_ExecutionMode", "Recompiler");
    set_option("swanstation_CPU_FastmemMode", "LUT");
}
#endif

static void apply_settings_to_options(const Settings *s)
{
#if defined(PSXS5_VULKAN)
    if (core == &SWANSTATION)
    {
        apply_swanstation_options(s);
        return;
    }
    if (core == &BEETLE)
    {
        apply_beetle_options(s);
        return;
    }
#endif
    static const char *regions[] = {"auto", "NTSC", "PAL"};
    set_option("pcsx_rearmed_region", regions[s->region % REGION_COUNT]);
    set_option("pcsx_rearmed_bios", s->force_hle ? "HLE" : "auto");
    set_option("pcsx_rearmed_dithering", s->dithering ? "enabled" : "disabled");
    set_option("pcsx_rearmed_cd_turbo", s->cd_fast && !(game_fixes & GDB_NO_CD_SPEEDUP) ? "enabled" : "disabled");
    set_option("pcsx_rearmed_rgb32_output", "enabled");
    set_option("pcsx_rearmed_memcard1", "serial");   /* one card per game, managed by the core */
    set_option("pcsx_rearmed_show_bios_bootlogo", s->boot_intro ? "enabled" : "disabled");
    set_option("pcsx_rearmed_vibration", "enabled");
    set_option("pcsx_rearmed_display_fps_v2", "disabled");
    /* 2x internal resolution: the enhanced GPU renders the 3D scene at double size. */
    set_option("pcsx_rearmed_neon_enhancement_enable", s->internal_res >= 2 ? "enabled" : "disabled");
    set_option("pcsx_rearmed_neon_enhancement_no_main", "disabled");
    /* players 3 and 4 through a multitap in port 1 */
    set_option("pcsx_rearmed_multitap", s->multitap ? "port 1" : "disabled");
    /* the widescreen codes need the picture's sides drawn */
    set_option("pcsx_rearmed_show_overscan", s->widescreen ? "hack" : "disabled");
    /* the emulated CPU's speed: auto is about 57 % of a real PS1's cycles */
    static const char *const clock[] = {"auto", "75", "100"};
    set_option("pcsx_rearmed_psxclock", clock[s->overclock >= 0 && s->overclock <= 2 ? s->overclock : 0]);
}

/* ---------------------------------------------------------------- Vulkan rendering */

#if defined(PSXS5_VULKAN)
/* Beetle PSX HW renders through Vulkan: it asks for a Vulkan context
 * (SET_HW_RENDER), creates the device itself through the negotiation
 * interface, and hands over a finished image each frame (set_image). The
 * screen (vk_present.c) moves onto that device and draws the image. */
static struct retro_hw_render_callback hw;
static bool hw_requested, hw_running;
static const struct retro_hw_render_context_negotiation_interface_vulkan *negotiation;
static struct retro_hw_render_interface_vulkan hw_interface;

static void hw_set_image(void *handle, const struct retro_vulkan_image *image, uint32_t num_semaphores,
                         const VkSemaphore *semaphores, uint32_t src_queue_family)
{
    (void)handle, (void)num_semaphores, (void)semaphores, (void)src_queue_family;
    vkp_set_game_image(image ? image->create_info.image : VK_NULL_HANDLE, image ? image->image_view : VK_NULL_HANDLE,
                       image ? image->image_layout : VK_IMAGE_LAYOUT_UNDEFINED);
}
static uint32_t hw_get_sync_index(void *handle)
{
    (void)handle;
    return vkp_sync_index();
}
static uint32_t hw_get_sync_index_mask(void *handle)
{
    (void)handle;
    return vkp_sync_index_mask();
}
static void hw_wait_sync_index(void *handle)
{
    (void)handle;
    vkp_wait_sync_index();
}
static void hw_set_command_buffers(void *handle, uint32_t num, const VkCommandBuffer *cmd)
{
    (void)handle, (void)num, (void)cmd; /* Beetle submits its own */
}
static void hw_queue_noop(void *handle)
{
    (void)handle; /* one thread submits: no lock needed */
}
static void hw_set_signal_semaphore(void *handle, VkSemaphore semaphore)
{
    (void)handle, (void)semaphore;
}

/* After retro_load_game: the core's device, the screen moved onto it, then
 * context_reset builds the renderer. */
static bool hw_start(char *error, size_t size)
{
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkSurfaceKHR surface;
    PFN_vkGetInstanceProcAddr gipa;
    vkp_hw_context(&instance, &gpu, &surface, &gipa);
    struct retro_vulkan_context context;
    memset(&context, 0, sizeof(context));
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (!negotiation || !negotiation->create_device ||
        !negotiation->create_device(&context, instance, gpu, surface, gipa, extensions, 1, NULL, 0, NULL))
    {
        snprintf(error, size, "Beetle could not create its Vulkan device.");
        return false;
    }
    char why[160];
    if (!vkp_adopt_device(context.device, context.queue, context.queue_family_index, why, sizeof(why)))
    {
        snprintf(error, size, "The screen could not move to Beetle's device: %s", why);
        return false;
    }
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    PFN_vkGetDeviceProcAddr gdpa;
    vkp_hw_device(&device, &queue, &family, &gdpa);
    memset(&hw_interface, 0, sizeof(hw_interface));
    hw_interface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    hw_interface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    hw_interface.instance = instance;
    hw_interface.gpu = context.gpu ? context.gpu : gpu;
    hw_interface.device = device;
    hw_interface.get_device_proc_addr = gdpa;
    hw_interface.get_instance_proc_addr = gipa;
    hw_interface.queue = queue;
    hw_interface.queue_index = family;
    hw_interface.set_image = hw_set_image;
    hw_interface.get_sync_index = hw_get_sync_index;
    hw_interface.get_sync_index_mask = hw_get_sync_index_mask;
    hw_interface.set_command_buffers = hw_set_command_buffers;
    hw_interface.wait_sync_index = hw_wait_sync_index;
    hw_interface.lock_queue = hw_queue_noop;
    hw_interface.unlock_queue = hw_queue_noop;
    hw_interface.set_signal_semaphore = hw_set_signal_semaphore;
    hw_running = true;
    if (hw.context_reset)
        hw.context_reset();
    psxs5_log("host: Beetle renders through Vulkan");
    return true;
}

/* The core asked for new geometry (internal resolution...): a new device
 * and renderer, between frames. */
static void hw_restart(void)
{
    if (hw.context_destroy)
        hw.context_destroy();
    hw_running = false;
    vkp_set_game_image(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
    char error[200];
    if (!hw_start(error, sizeof(error)))
        psxs5_log("host: could not rebuild Beetle's renderer: %s", error);
    else
        psxs5_log("host: Beetle's renderer rebuilt for %ux%u", av_info.geometry.max_width,
                  av_info.geometry.max_height);
}

static void hw_stop(void)
{
    if (hw_running && hw.context_destroy)
        hw.context_destroy();
    hw_running = false;
    hw_requested = false;
    negotiation = NULL;
    vkp_set_game_image(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
}
#endif

/* ---------------------------------------------------------------- callbacks */

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_INFO)
        return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    psxs5_log("core: %s", line);
}

static float rumble_scale = 1.0f; /* Settings > Controls > Vibration */
static int gun_device;           /* 0 a pad, 1 GunCon, 2 Justifier in port 1 */

void host_set_gun(int device)
{
    gun_device = device;
}

static int special_device; /* 0 pads, 1 NeGcon in every port, 2 a mouse in port 1 */

void host_set_special(int device)
{
    special_device = device;
}

void host_set_fixes(unsigned flags)
{
    game_fixes = flags;
}
static int rumble_feel;          /* Settings > Controls > Rumble feel */
static uint16_t rumble_strong[PSXS5_MAX_PADS], rumble_weak[PSXS5_MAX_PADS];

/* Soft: lighter, the big motor tamed. Punchy: weak rumbles lifted so short
 * hits are felt, strong ones unchanged. */
static uint16_t feel(uint16_t v, bool big)
{
    float x = v / 65535.0f;
    if (rumble_feel == 1)
        x *= big ? 0.55f : 0.8f;
    else if (rumble_feel >= 2 && x > 0.0f)
        x = sqrtf(x);
    x *= rumble_scale;
    return (uint16_t)(x > 1.0f ? 65535.0f : x * 65535.0f);
}

static bool RETRO_CALLCONV rumble_cb(unsigned port, enum retro_rumble_effect effect,
                                     uint16_t strength)
{
    if (port >= PSXS5_MAX_PADS)
        return false;
    if (speculative)
        return true; /* only the real frame's rumble counts */
    if (effect == RETRO_RUMBLE_STRONG)
        rumble_strong[port] = strength;
    else
        rumble_weak[port] = strength;
    plat_rumble((int)port, feel(rumble_strong[port], true), feel(rumble_weak[port], false));
    return true;
}

float host_rumble_level(int port)
{
    if (port < 0 || port >= PSXS5_MAX_PADS || !loaded || rumble_scale <= 0.0f)
        return 0.0f;
    float s = rumble_strong[port] / 65535.0f, w = rumble_weak[port] / 65535.0f;
    return s > w ? s : w;
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0; /* core falls back to SET_VARIABLES */
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        register_variables((const struct retro_variable *)data);
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *var = (struct retro_variable *)data;
        Option *opt = find_option(var->key);
        var->value = opt ? opt->value : NULL;
        return opt != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = options_dirty;
        options_dirty = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format fmt = *(const enum retro_pixel_format *)data;
        if (fmt > RETRO_PIXEL_FORMAT_RGB565)
            return false;
        pixel_format = fmt;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = host_paths->bios;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = host_paths->saves;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        ((struct retro_rumble_interface *)data)->set_rumble_state = rumble_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        memset(&disk, 0, sizeof(disk));
        memcpy(&disk, data, sizeof(struct retro_disk_control_callback));
        disk_available = true;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        memcpy(&disk, data, sizeof(disk));
        disk_available = true;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        const struct retro_system_av_info *next = data;
#if defined(PSXS5_VULKAN)
        /* Beetle's Vulkan renderer reads its internal resolution only when
         * it is rebuilt: like RetroArch, rebuild the context right away. Not
         * for a timing change alone (each switch between interlaced and
         * progressive video, as from the white Sony screen to the PlayStation
         * logo): a rebuild there stalled and lost frames */
        bool resized = next->geometry.max_width != av_info.geometry.max_width ||
                       next->geometry.max_height != av_info.geometry.max_height;
        av_info = *next;
        if (hw_running && resized)
            hw_restart();
#else
        av_info = *next;
#endif
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        av_info.geometry = *(const struct retro_game_geometry *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    {
        /* kept for RetroAchievements: where PS1 RAM and scratchpad live */
        const struct retro_memory_map *m = data;
        memory_map.num_descriptors = m->num_descriptors < 32 ? m->num_descriptors : 32;
        memcpy(memory_descriptors, m->descriptors,
               memory_map.num_descriptors * sizeof(struct retro_memory_descriptor));
        memory_map.descriptors = memory_descriptors;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        /* Beetle then gives its real state size (about 5 MB) rather than a
         * flat 16 MB: rewind's 40 states took 640 MB of the 1 GB heap, and
         * save states failed for want of memory */
        if (data)
            *(uint64_t *)data |= RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        psxs5_log("core message: %s", ((const struct retro_message *)data)->msg);
        return true;
#if defined(PSXS5_VULKAN)
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *(unsigned *)data = RETRO_HW_CONTEXT_VULKAN;
        return vkp_describe()[0] != '\0';
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        struct retro_hw_render_callback *cb = data;
        if ((core != &BEETLE && core != &SWANSTATION) || cb->context_type != RETRO_HW_CONTEXT_VULKAN || !vkp_describe()[0])
            return false;
        hw = *cb;
        hw_requested = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    {
        const struct retro_hw_render_context_negotiation_interface *i = data;
        if (i->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
            return false;
        negotiation = data;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
    {
        struct retro_hw_render_context_negotiation_interface *i = data;
        if (i->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
            return false;
        i->interface_version = 1; /* create_device, not create_device2 */
        return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        if (!hw_running)
            return false;
        *(const struct retro_hw_render_interface **)data = (const struct retro_hw_render_interface *)&hw_interface;
        return true;
#endif
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
        psxs5_log("core message: %s", ((const struct retro_message_ext *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    default:
        return false;
    }
}

static void RETRO_CALLCONV video_cb(const void *data, unsigned width, unsigned height,
                                    size_t pitch)
{
    if (!data)
        return; /* duplicate frame: keep showing the previous one */
    if (data == RETRO_HW_FRAME_BUFFER_VALID)
    {
        /* rendered on the GPU: the image went through set_image */
        frame_data = NULL;
        frame_w = width;
        frame_h = height;
        frame_fresh = true;
        return;
    }
    frame_data = data;
    frame_w = width;
    frame_h = height;
    frame_pitch = pitch;
    frame_fresh = true;
}

void host_set_speculative(bool on)
{
    speculative = on;
}

static void RETRO_CALLCONV audio_cb(int16_t left, int16_t right)
{
    if (speculative)
        return;
    int16_t frame[2] = {left, right};
    plat_audio_push(frame, 1);
}

static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames)
{
    if (!speculative)
        plat_audio_push(data, frames);
    return frames;
}

static void RETRO_CALLCONV input_poll_cb(void)
{
}

static int16_t RETRO_CALLCONV input_state_cb(unsigned port, unsigned device, unsigned index,
                                             unsigned id)
{
    if (port >= PSXS5_MAX_PADS || !pad_state[port].connected)
        return 0;
    const PadState *p = &pad_state[port];
    switch (device & RETRO_DEVICE_MASK)
    {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return (int16_t)(p->buttons & 0xffff);
        return id < 16 ? (int16_t)((p->buttons >> id) & 1) : 0;
    case RETRO_DEVICE_ANALOG:
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? p->lx : p->ly;
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? p->rx : p->ry;
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
        {
            /* how far a button is pressed (the NeGcon's I, II and L): the triggers'
             * travel, full for the others */
            if (id == RETRO_DEVICE_ID_JOYPAD_R2)
                return (int16_t)(p->r2 * 128 + (p->r2 >> 1));
            if (id == RETRO_DEVICE_ID_JOYPAD_L2)
                return (int16_t)(p->l2 * 128 + (p->l2 >> 1));
            return id < 16 && ((p->buttons >> id) & 1) ? 0x7fff : 0;
        }
        return 0;
    case RETRO_DEVICE_MOUSE:
        switch (id)
        {
        case RETRO_DEVICE_ID_MOUSE_X: return p->mouse_dx;
        case RETRO_DEVICE_ID_MOUSE_Y: return p->mouse_dy;
        case RETRO_DEVICE_ID_MOUSE_LEFT: return ((p->buttons >> BTN_R2) | (p->buttons >> BTN_CROSS)) & 1;
        case RETRO_DEVICE_ID_MOUSE_RIGHT: return ((p->buttons >> BTN_L2) | (p->buttons >> BTN_CIRCLE)) & 1;
        default: return 0;
        }
    case RETRO_DEVICE_LIGHTGUN:
        /* the controller as a gun: aimed by PSXS5 (controls.c), R2 fires,
         * L2 or Square reloads (a shot off the screen), Cross / Circle are A / B */
        switch (id)
        {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return p->gun_x;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return p->gun_y;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return p->gun_offscreen;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return (p->buttons >> BTN_R2) & 1;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD: return ((p->buttons >> BTN_L2) | (p->buttons >> BTN_SQUARE)) & 1;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return (p->buttons >> BTN_CROSS) & 1;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return (p->buttons >> BTN_CIRCLE) & 1;
        case RETRO_DEVICE_ID_LIGHTGUN_START: return (p->buttons >> BTN_START) & 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- memory card */

/* One card per game, whichever emulator runs it: <saves>/<serial>_1.mcd,
 * the file PCSX-ReARMed writes itself ("serial" cards) and the memory card
 * manager shows. Beetle's card is the frontend's (SAVE_RAM): loaded from that
 * file after retro_load_game, written back when it changes. */
static char card_path[PSXS5_PATH_MAX];
static uint8_t card_saved[128 * 1024];
static int card_check;

static void card_prepare(const char *serial, const char *game_path)
{
    card_path[0] = '\0';
    char name[200], path[PSXS5_PATH_MAX];
    if (serial[0])
        snprintf(name, sizeof(name), "%s_1.mcd", serial);
    else
    {
        /* no serial known: the game file's name */
        const char *base = strrchr(game_path, '/');
        snprintf(name, sizeof(name), "%.150s", base ? base + 1 : game_path);
        char *dot = strrchr(name, '.');
        if (dot)
            *dot = '\0';
        strncat(name, "_1.mcd", sizeof(name) - strlen(name) - 1);
    }
    path_join(card_path, sizeof(card_path), host_paths->saves, name);
    /* PCSX names it as the disc spells its ID, sometimes lower case */
    FILE *f = fopen(card_path, "rb");
    if (f)
    {
        fclose(f);
        return;
    }
    for (char *c = name; *c; ++c)
        *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    path_join(path, sizeof(path), host_paths->saves, name);
    if ((f = fopen(path, "rb")) != NULL)
    {
        fclose(f);
        str_copy(card_path, sizeof(card_path), path);
    }
}

static uint8_t *beetle_card(void)
{
#if defined(PSXS5_VULKAN)
    if (core == &BEETLE && card_path[0] && core->get_memory_size(RETRO_MEMORY_SAVE_RAM) == sizeof(card_saved))
        return core->get_memory_data(RETRO_MEMORY_SAVE_RAM);
#endif
    return NULL;
}

static void card_load(void)
{
    uint8_t *card = beetle_card();
    if (!card)
        return;
    FILE *f = fopen(card_path, "rb");
    if (f)
    {
        size_t n = fread(card, 1, sizeof(card_saved), f);
        fclose(f);
        psxs5_log("host: memory card %s (%s)", card_path, n == sizeof(card_saved) ? "loaded" : "short");
    }
    memcpy(card_saved, card, sizeof(card_saved));
}

static void card_flush(void)
{
    uint8_t *card = beetle_card();
    if (!card || memcmp(card, card_saved, sizeof(card_saved)) == 0)
        return;
    char temp[PSXS5_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.tmp", card_path);
    FILE *f = fopen(temp, "wb");
    bool ok = f && fwrite(card, 1, sizeof(card_saved), f) == sizeof(card_saved);
    if (f)
        ok = fclose(f) == 0 && ok;
    ok = ok && rename(temp, card_path) == 0;
    if (ok)
        memcpy(card_saved, card, sizeof(card_saved));
    psxs5_log("host: memory card %s %s", card_path, ok ? "saved" : "could not be saved");
}

/* ---------------------------------------------------------------- API */

#if defined(PSXS5_VULKAN)
/* Beetle needs a real BIOS of the disc's region, by one of the names it
 * looks for (libretro.c firmware_is_present). Region from the serial. */
static bool __attribute__((unused)) beetle_bios_present(const char *serial)
{
    static const char *const jp[] = {"scph5500.bin", "SCPH5500.bin", "SCPH5500.BIN", "SCPH-5500.bin",
                                     "SCPH-5500.BIN", NULL};
    static const char *const us[] = {"scph5501.bin", "SCPH5501.bin", "SCPH5501.BIN", "SCPH-5501.bin",
                                     "SCPH-5501.BIN", "scph5503.bin", "scph7003.bin", NULL};
    static const char *const eu[] = {"scph5502.bin", "SCPH5502.bin", "SCPH5502.BIN", "SCPH-5502.bin",
                                     "SCPH-5502.BIN", "scph5552.bin", NULL};
    const char *const *lists[3] = {us, eu, jp};
    int only = -1; /* unknown region: any BIOS will do */
    if (!strncmp(serial, "SLUS", 4) || !strncmp(serial, "SCUS", 4) || !strncmp(serial, "PAPX", 4))
        only = 0;
    else if (!strncmp(serial, "SLES", 4) || !strncmp(serial, "SCES", 4) || !strncmp(serial, "SCED", 4))
        only = 1;
    else if (serial[0] == 'S' || serial[0] == 'P') /* SLPS, SCPS, SLPM, SIPS, PCPX... */
        only = 2;
    for (int l = 0; l < 3; ++l)
    {
        if (only >= 0 && l != only)
            continue;
        for (const char *const *name = lists[l]; *name; ++name)
        {
            char path[PSXS5_PATH_MAX];
            path_join(path, sizeof(path), host_paths->bios, *name);
            FILE *f = fopen(path, "rb");
            if (f)
            {
                fclose(f);
                return true;
            }
        }
    }
    return false;
}
#endif

/* Which core runs a game, and why not Beetle when Automatic picks PCSX. */
static const CoreApi *pick_core(const Settings *settings, const char *serial, const char **why)
{
    *why = NULL;
    (void)settings, (void)serial;
#if defined(PSXS5_VULKAN)
    /* SwanStation is the only emulator offered (the Emulator setting is hidden); PCSX-ReARMed
     * only when the screen isn't drawn through Vulkan, as SwanStation renders on the GPU.
     * Beetle PSX HW and the old choice stay in the build: restore the Settings row to bring them back. */
    if (vkp_describe()[0])
        return &SWANSTATION;
    *why = "the screen isn't drawn through Vulkan";
#endif
    return &PCSX;
}

static const CoreApi *choose_core(const Settings *settings, const char *serial)
{
    const char *why;
    const CoreApi *c = pick_core(settings, serial, &why);
    if (why)
        psxs5_log("host: PCSX-ReARMed, as %s", why);
    return c;
}

const char *host_emulator_for(const Settings *settings, const char *serial, const char **why_not_beetle)
{
    if (!host_paths)
        host_paths = app_paths();
    return pick_core(settings, serial ? serial : "", why_not_beetle)->name;
}

const char *host_core_name(void)
{
    return core->name;
}

bool host_load(const char *game_path, const char *serial, const Paths *paths, const Settings *settings,
               char *error, size_t error_size)
{
    host_unload();
    host_paths = paths;
    str_copy(loading_path, sizeof(loading_path), game_path);
    core = choose_core(settings, serial ? serial : "");
    psxs5_log("host: emulator %s", core->name);
    {
        /* Beetle keeps its Vulkan pipeline cache in <saves>/Beetle PSX HW; without
         * the folder it can't, and every new effect compiles again (a stutter) */
        char cache_dir[PSXS5_PATH_MAX];
        path_join(cache_dir, sizeof(cache_dir), paths->saves, "Beetle PSX HW");
        make_dirs(cache_dir);
    }
    card_prepare(serial ? serial : "", game_path);
    option_count = 0;
    disk_available = false;
    lid_open_frames = 0;
    frame_data = NULL;
    pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    rumble_scale = settings->rumble ? (settings->rumble_strength + 1) * 0.25f : 0.0f;
    rumble_feel = settings->rumble_feel;
    memset(rumble_strong, 0, sizeof(rumble_strong));
    memset(rumble_weak, 0, sizeof(rumble_weak));
    apply_settings_to_options(settings);

#define STEP(s) (psxs5_log("host: %s", s), ps5_crash_step(s))
    STEP("retro_set_environment");
    core->set_environment(environment);
    core->set_video_refresh(video_cb);
    core->set_audio_sample(audio_cb);
    core->set_audio_sample_batch(audio_batch_cb);
    core->set_input_poll(input_poll_cb);
    core->set_input_state(input_state_cb);
    STEP("retro_init");
    core->init();
    if (core == &PCSX)
        psxs5_set_patches_dir(patches_dir);

    STEP("retro_load_game");
    struct retro_game_info info = {game_path, NULL, 0, NULL};
    if (!core->load_game(&info))
    {
        snprintf(error, error_size, "The core could not load this game.");
        core->deinit();
        return false;
    }
#if defined(PSXS5_VULKAN)
    if (hw_requested && !hw_start(error, error_size))
    {
        psxs5_log("%s", error);
        hw_stop();
        core->unload_game();
        core->deinit();
        return false;
    }
#endif
    STEP("retro_get_system_av_info");
    core->get_system_av_info(&av_info);
    /* DualShock starts in digital mode, so it is also safe for digital-only games. */
    unsigned device = settings->analog ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)
                                       : RETRO_DEVICE_JOYPAD;
    multitap = settings->multitap;
    for (unsigned port = 0; port < (multitap ? 4u : 2u); ++port)
        core->set_controller_port_device(port, device);
    /* NeGcon (analog subclass 3 in Beetle, 2 in PCSX-ReARMed) in every port, or a mouse in port 1 */
    if (special_device == 1)
        for (unsigned port = 0; port < (multitap ? 4u : 2u); ++port)
            core->set_controller_port_device(port, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, core == &PCSX ? 2 : 3));
    if (special_device == 2)
        core->set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0));
    if (special_device)
        psxs5_log("host: %s", special_device == 1 ? "NeGcon in each port" : "a mouse in port 1");
    /* a light gun in port 1: GunCon is subclass 0, the Justifier 1, in both cores */
    if (gun_device)
    {
        core->set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, gun_device - 1));
        psxs5_log("host: port 1 is a %s", gun_device == 1 ? "GunCon" : "Justifier");
    }
    loaded = true;
    state_size = 0;
    card_load();
    STEP("running");
#undef STEP
    psxs5_log("loaded %s: %.3f fps, %.0f Hz, base %ux%u", game_path, av_info.timing.fps,
              av_info.timing.sample_rate, av_info.geometry.base_width,
              av_info.geometry.base_height);
    return true;
}

void host_unload(void)
{
    if (!loaded)
        return;
    card_flush();
    core->unload_game();
#if defined(PSXS5_VULKAN)
    hw_stop();
#endif
    core->deinit();
    loaded = false;
    state_size = 0;
    frame_data = NULL;
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
        plat_rumble(i, 0, 0);
}

bool host_loaded(void)
{
    return loaded;
}

void *host_memory_data(unsigned id)
{
    return loaded ? core->get_memory_data(id) : NULL;
}

size_t host_memory_size(unsigned id)
{
    return loaded ? core->get_memory_size(id) : 0;
}

const struct retro_memory_map *host_memory_map(void)
{
    return loaded && memory_map.num_descriptors ? &memory_map : NULL;
}

/* The core's CD layer (libpcsxcore/cdrom-async.h): reads through whatever
 * image format is loaded (bin/cue, CHD, PBP...). */
int cdra_readTrack(const unsigned char *time);
void *cdra_getBuffer(void);
int cdra_init(void);
int cdra_open(void);
void cdra_close(void);
void set_cd_image(const char *fname); /* PCSX-ReARMed frontend/main.c */

/* When another core runs the game, PCSX-ReARMed's CD layer still reads every
 * image format (bin/cue, CHD, PBP...): open it just to hash the disc. */
static bool hash_disc_open;

bool host_hash_disc_begin(const char *disc_path)
{
    if (loaded && core == &PCSX)
        return true; /* the running core's disc */
    host_hash_disc_end();
    cdra_init();
    set_cd_image(disc_path);
    hash_disc_open = cdra_open() == 0;
    if (!hash_disc_open)
        psxs5_log("ra: could not open %s to identify it", disc_path);
    return hash_disc_open;
}

void host_hash_disc_end(void)
{
    if (hash_disc_open)
        cdra_close();
    hash_disc_open = false;
}

bool host_read_sector(uint32_t lba, uint8_t out[2048])
{
    if (!hash_disc_open && (!loaded || core != &PCSX))
        return false;
    unsigned abs = lba + 150; /* sector 0 is at 00:02:00 */
    /* minute, second, frame as plain numbers: the core's cdra_readTrack takes
     * them through msf2sec, not as the BCD the PS1's CD commands use */
    unsigned char time[3] = {(unsigned char)(abs / 75 / 60), (unsigned char)(abs / 75 % 60),
                             (unsigned char)(abs % 75)};
    if (cdra_readTrack(time) != 0)
        return false;
    const uint8_t *buf = cdra_getBuffer();
    if (!buf)
        return false;
    memcpy(out, buf + 12, 2048); /* skip MSF/mode + subheader: Mode 2 Form 1 data */
    return true;
}

int padGetMode(unsigned int index); /* core, added by tools/patches/pcsx_rearmed-padgetmode.patch */

bool host_pad_digital(int port)
{
    return loaded && core == &PCSX && padGetMode((unsigned)port) == 0;
}

void host_set_pads(const PadState pads[PSXS5_MAX_PADS])
{
    memcpy(pad_state, pads, sizeof(pad_state));
}

void host_run_frame(void)
{
    if (!loaded)
        return;
    if (lid_open_frames > 0 && --lid_open_frames == 0 && disk_available)
        disk.set_eject_state(false); /* the disc change, completed */
    core->run();
    if (++card_check >= 120) /* every 2 s: games write a save in a burst */
    {
        card_check = 0;
        card_flush();
    }
}

void host_reset(void)
{
    if (loaded)
        core->reset();
}

void host_apply_settings(const Settings *settings)
{
    rumble_scale = settings->rumble ? (settings->rumble_strength + 1) * 0.25f : 0.0f;
    rumble_feel = settings->rumble_feel;
    apply_settings_to_options(settings);
}

double host_fps(void)
{
    return av_info.timing.fps > 1.0 ? av_info.timing.fps : 59.94;
}

int host_sample_rate(void)
{
    return av_info.timing.sample_rate > 1000.0 ? (int)(av_info.timing.sample_rate + 0.5) : 44100;
}

float host_aspect(void)
{
    return av_info.geometry.aspect_ratio > 0.0f ? av_info.geometry.aspect_ratio : 4.0f / 3.0f;
}

const void *host_frame(int *width, int *height, size_t *pitch, int *format, bool *fresh)
{
    *width = (int)frame_w;
    *height = (int)frame_h;
    *pitch = frame_pitch;
    *format = (int)pixel_format;
    *fresh = frame_fresh;
    frame_fresh = false;
    return frame_data;
}

bool host_save_state(const char *path)
{
    size_t size = host_state_size();
    if (size == 0)
    {
        psxs5_log("host: save state: the emulator gives no state size");
        return false;
    }
    void *buffer = malloc(size);
    if (!buffer)
    {
        psxs5_log("host: save state: no memory for %zu bytes", size);
        return false;
    }
    bool ok = core->serialize(buffer, size);
    if (!ok)
        psxs5_log("host: save state: the emulator couldn't write its state (%zu bytes)", size);
    else
    {
        char temp[PSXS5_PATH_MAX];
        snprintf(temp, sizeof(temp), "%s.tmp", path);
        FILE *f = fopen(temp, "wb");
        ok = f && fwrite(buffer, 1, size, f) == size;
        if (f)
            ok = (fclose(f) == 0) && ok;
        if (!ok)
            psxs5_log("host: save state: couldn't write %s (errno %d)", temp, errno);
        ok = ok && rename(temp, path) == 0;
        if (!ok)
            remove(temp);
    }
    free(buffer);
    return ok;
}

void host_set_patches_dir(const char *dir)
{
    str_copy(patches_dir, sizeof(patches_dir), dir ? dir : "");
}

size_t host_state_size(void)
{
    if (!loaded)
        return 0;
    if (state_size == 0)
    {
        /* Beetle measures by saving a whole state: once per game, with room
         * to spare in case a later state is a little larger */
        size_t size = core->serialize_size();
        state_size = size && core != &PCSX ? size + 512 * 1024 : size; /* Beetle */
        psxs5_log("host: states take %zu KB", state_size / 1024);
    }
    return state_size;
}

bool host_serialize(void *buffer, size_t size)
{
    return loaded && core->serialize(buffer, size);
}

bool host_unserialize(const void *buffer, size_t size)
{
    if (!loaded || !core->unserialize(buffer, size))
        return false;
    plat_audio_clear();
    return true;
}

bool host_capture(uint8_t *rgba, int w, int h)
{
#if defined(PSXS5_VULKAN)
    if (loaded && hw_running && !frame_data && frame_w && frame_h) /* rendered on the GPU */
        return vkp_capture_game(rgba, w, h, (int)frame_w, (int)frame_h);
#endif
    if (!loaded || !frame_data || frame_w == 0 || frame_h == 0)
        return false;
    for (int y = 0; y < h; ++y)
    {
        unsigned sy = (unsigned)((y * 2 + 1) * frame_h / (2 * (unsigned)h));
        const uint8_t *row = (const uint8_t *)frame_data + sy * frame_pitch;
        for (int x = 0; x < w; ++x)
        {
            unsigned sx = (unsigned)((x * 2 + 1) * frame_w / (2 * (unsigned)w));
            uint8_t *o = &rgba[((size_t)y * w + x) * 4];
            if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
            {
                uint32_t c = ((const uint32_t *)row)[sx];
                o[0] = (c >> 16) & 0xff, o[1] = (c >> 8) & 0xff, o[2] = c & 0xff;
            }
            else
            {
                uint16_t c = ((const uint16_t *)row)[sx];
                if (pixel_format == RETRO_PIXEL_FORMAT_RGB565)
                    o[0] = (c >> 11) << 3, o[1] = ((c >> 5) & 63) << 2, o[2] = (c & 31) << 3;
                else
                    o[0] = ((c >> 10) & 31) << 3, o[1] = ((c >> 5) & 31) << 3, o[2] = (c & 31) << 3;
            }
            o[3] = 255;
        }
    }
    return true;
}

bool host_load_state(const char *path)
{
    if (!loaded)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    void *buffer = size > 0 ? malloc((size_t)size) : NULL;
    if (buffer && fread(buffer, 1, (size_t)size, f) == (size_t)size)
        ok = core->unserialize(buffer, (size_t)size);
    free(buffer);
    fclose(f);
    if (ok)
        plat_audio_clear();
    return ok;
}

void host_beetle_widescreen(bool on)
{
#if defined(PSXS5_VULKAN)
    if (game_fixes & GDB_NO_WIDESCREEN)
        on = false; /* the renderer's widescreen breaks this game */
    if (loaded && core == &BEETLE)
        set_option("beetle_psx_hw_widescreen_hack", on ? "enabled" : "disabled");
#else
    (void)on;
#endif
}

void host_cheat_reset(void)
{
    if (loaded)
        core->cheat_reset();
}

void host_cheat_set(unsigned index, const char *code)
{
    if (loaded)
        core->cheat_set(index, true, code);
}

int host_disc_count(void)
{
    return disk_available && disk.get_num_images ? (int)disk.get_num_images() : 1;
}

int host_disc_index(void)
{
    return disk_available && disk.get_image_index ? (int)disk.get_image_index() : 0;
}

bool host_disc_select(int index)
{
    if (!disk_available || index < 0 || index >= host_disc_count())
        return false;
    /* Open the lid and swap; it closes about a second later (host_run_frame),
     * as a hand would: closed at once, games didn't notice the change. */
    if (!disk.get_eject_state() && !disk.set_eject_state(true))
        return false;
    bool ok = disk.set_image_index((unsigned)index);
    lid_open_frames = 60;
    return ok;
}
