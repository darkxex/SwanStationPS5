/* SwanStationPS5 - frame generation (AMD FSR 3's frame interpolation, from PS5SX2: third_party/framegen),
 * for the presenter in C. From two of the game's frames, the one halfway between them.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgInterpolator FgInterpolator;

/* frames of width x height in `format`; NULL (and `error`) when it can't be made */
FgInterpolator *ssfg_create(PFN_vkVoidFunction (*get_instance_proc)(VkInstance, const char *), VkInstance instance,
                                VkPhysicalDevice gpu, VkDevice device, uint32_t width, uint32_t height, VkFormat format,
                                char *error, size_t error_size);
void ssfg_destroy(FgInterpolator *fg);

/* each new frame of the game: prepare, copy the frame into frame_image (GENERAL layout), record;
 * record says whether output_view holds the frame halfway to the previous one */
void ssfg_prepare(FgInterpolator *fg, VkCommandBuffer cmd);
VkImage ssfg_frame_image(FgInterpolator *fg);
VkImageView ssfg_frame_view(FgInterpolator *fg);
bool ssfg_record(FgInterpolator *fg, VkCommandBuffer cmd, bool reset);
VkImageView ssfg_output_view(FgInterpolator *fg);
/* once the frame was shown: it becomes the previous one */
void ssfg_advance(FgInterpolator *fg);

/* The pacing (third_party/framegen/OrbisFrameGenPacing.h, PS5SX2's): from the game's own frames, counted in the
 * console's vsyncs, against the display's refreshes, whether frames are generated and how many presents each gets. */
typedef struct FgPacing FgPacing;
typedef struct
{
    bool engaged;       /* frames are generated: the interpolator takes this new frame */
    bool reset;         /* the interpolator starts over (a gap, a pause) */
    uint32_t presents;  /* presents of the generated frame before the game's, for this frame */
    int state;          /* 0 warming, 1 no room, 2 unsteady, 3 not nominal, 4 paused, 5 generating */
    double vsyncs;      /* the median of the last frames' length, in vsyncs */
    double refreshes;   /* and in the display's refreshes */
} FgDecision;

FgPacing *ssfg_pacing_create(void);
void ssfg_pacing_destroy(FgPacing *pacing);
/* One new frame of the game: vsyncs since the last new one, presents already made in them, the console's and the
 * display's rates (Hz), the time (s), the emulation's speed (% of its target) and whether the speed is normal. */
FgDecision ssfg_pacing_frame(FgPacing *pacing, uint32_t vsyncs, uint32_t already, double core_hz, double display_hz,
                             double now, double speed, bool nominal);
const char *ssfg_pacing_state_name(int state);
/* After a frame: the safety net's news for the log (NULL for none) */
const char *ssfg_pacing_event(FgPacing *pacing);

#ifdef __cplusplus
}
#endif
