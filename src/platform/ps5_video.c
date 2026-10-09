/*
 * SwanStationPS5 - PS5 screen output without SDL's video driver.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PacBrew's SDL2 PS5 video driver can't hand out a window surface (its
 * CreateWindowFramebuffer leaves format/pixels/pitch unset, which SDL turns
 * into "Out of memory") and registers no render driver. SwanStationPS5 therefore
 * draws with SDL's software renderer into its own linear buffer and shows
 * it here, using the same VideoOut sequence as SDL's driver init and the
 * boilerplate demo: two direct-memory framebuffers in the 64 KiB tiled
 * layout, filled by the CPU, flushed, flipped.
 */
#if defined(__PROSPERO__)
#include "ps5_video.h"

#include <stdint.h>
#include <string.h>

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_address, size_t alignment);
int sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t flip_mode,
                          int64_t flip_argument);
int sceVideoOutWaitVblank(int32_t handle);

typedef struct
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
} VideoBuffer;

typedef struct
{
    uint8_t reserved[80];
} VideoAttribute;

void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute, uint64_t pixel_format,
                                    uint32_t tiling_mode, uint32_t width, uint32_t height,
                                    uint64_t option, uint32_t dcc_control, uint64_t dcc_clear_color);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index_start,
                                VideoBuffer *buffers, int32_t buffer_count,
                                VideoAttribute *attribute, int32_t category, void *option);

#define FRAME_BYTES 0x1000000u /* one 1920x1080 tiled frame, rounded up */
#define MEMORY_ALIGN 0x200000u
#define MEMORY_TYPE_WC_GARLIC 3
#define MAP_CPU_GPU_RW 0x33
#define PIXEL_FORMAT_RGBA8_SRGB 0x8000000022000000ull

int sceVideoOutIsFlipPending(int32_t handle);

#define BUFFERS 3 /* draw into one while another waits for vblank */

static int handle = -1;
static uint8_t *frames[BUFFERS];
static int last_submitted;
static uint64_t flip_count = 1;
/* Inside a 64 KiB / 128x128 block, a pixel's tiled offset is
 * (row bits) XOR (column bits). `inverse` maps each 4-byte slot of a block,
 * in memory order, back to its (y << 7 | x): the block is then written
 * strictly sequentially, which write-combined memory needs to be fast
 * (scattered 4-byte writes made presenting a frame take ~14 ms). */
static uint16_t inverse[16384];

static void build_swizzle(void)
{
    for (uint32_t y = 0; y < 128; ++y)
    {
        uint32_t row = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^
                       ((y << 8) & 0x4000u);
        for (uint32_t x = 0; x < 128; ++x)
        {
            uint32_t col = ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^
                           ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
            inverse[(row ^ col) >> 2] = (uint16_t)(y << 7 | x);
        }
    }
}

bool ps5_video_open(char *error, size_t size)
{
    build_swizzle();
    handle = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (handle < 0)
    {
        snprintf(error, size, "sceVideoOutOpen failed (0x%x)", (unsigned)handle);
        return false;
    }
    const size_t total = (size_t)FRAME_BYTES * BUFFERS;
    int64_t physical = 0;
    int rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), total,
                                           MEMORY_ALIGN, MEMORY_TYPE_WC_GARLIC, &physical);
    if (rc < 0)
    {
        snprintf(error, size, "direct memory allocation failed (0x%x)", (unsigned)rc);
        return false;
    }
    void *mapped = NULL;
    rc = sceKernelMapDirectMemory(&mapped, total, MAP_CPU_GPU_RW, 0, physical, MEMORY_ALIGN);
    if (rc < 0)
    {
        snprintf(error, size, "direct memory mapping failed (0x%x)", (unsigned)rc);
        return false;
    }
    VideoBuffer buffers[BUFFERS];
    for (int i = 0; i < BUFFERS; ++i)
    {
        frames[i] = (uint8_t *)mapped + (size_t)i * FRAME_BYTES;
        buffers[i] = (VideoBuffer){frames[i], NULL, NULL, NULL};
    }
    memset(mapped, 0, total);
    VideoAttribute attribute;
    memset(&attribute, 0, sizeof(attribute));
    sceVideoOutSetFlipRate(handle, 0);
    sceVideoOutSetBufferAttribute2(&attribute, PIXEL_FORMAT_RGBA8_SRGB, 0, PS5_SCREEN_W,
                                   PS5_SCREEN_H, 0, 0, 0);
    rc = sceVideoOutRegisterBuffers2(handle, 0, 0, buffers, BUFFERS, &attribute, 0, NULL);
    if (rc < 0)
    {
        snprintf(error, size, "sceVideoOutRegisterBuffers2 failed (0x%x)", (unsigned)rc);
        return false;
    }
    sceVideoOutSubmitFlip(handle, 0, 1, flip_count++);
    return true;
}

void ps5_video_present(const uint32_t *pixels, size_t pitch_bytes)
{
    if (handle < 0)
        return;
    /* Of three buffers, the next one is neither on screen nor queued, even
     * while the previous flip still waits for its vblank. */
    const int target = (last_submitted + 1) % BUFFERS;
    uint32_t *dst = (uint32_t *)frames[target];
    const size_t pitch = pitch_bytes / 4;
    const uint32_t blocks_per_row = PS5_SCREEN_W / 128;              /* 15 */
    const uint32_t block_rows = (PS5_SCREEN_H + 127) / 128;          /* 9: rows up to 1151 */
    for (uint32_t by = 0; by < block_rows; ++by)
        for (uint32_t bx = 0; bx < blocks_per_row; ++bx)
        {
            /* the canvas has PS5_CANVAS_ROWS rows, so the last block row can be read whole */
            const uint32_t *src = pixels + (size_t)by * 128 * pitch + (size_t)bx * 128;
            uint32_t *out = dst + ((size_t)(by * blocks_per_row + bx) << 14);
            for (uint32_t i = 0; i < 16384; ++i)
            {
                const uint32_t v = inverse[i];
                out[i] = src[(size_t)(v >> 7) * pitch + (v & 127)];
            }
        }
    /* write-combined memory: a store fence makes the frame visible */
    __asm__ volatile("sfence" ::: "memory");
    /* never queue a second flip: wait for the previous one to happen first */
    for (int spins = 0; sceVideoOutIsFlipPending(handle) > 0 && spins < 4; ++spins)
        sceVideoOutWaitVblank(handle);
    sceVideoOutSubmitFlip(handle, target, 1, (int64_t)flip_count++);
    last_submitted = target;
}
#endif
