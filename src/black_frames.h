/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SWANSTATIONPS5_BLACK_FRAMES_H
#define SWANSTATIONPS5_BLACK_FRAMES_H

#include <stdbool.h>
#include <stdint.h>

static inline uint8_t black_frames_dim(int intensity)
{
    intensity = intensity < 0 ? 0 : intensity > 100 ? 100 : intensity;
    return (uint8_t)((255 * (100 - intensity) + 50) / 100);
}

/* Use the actual refresh rate; PAL cannot alternate evenly at 120 Hz. */
static inline bool black_frames_active(bool enabled, bool video_sync, int framegen, double display_hz, double core_hz)
{
    return enabled && video_sync && !framegen && display_hz > 100.0 && core_hz > 20.0 &&
           core_hz / display_hz > 0.495 && core_hz / display_hz < 0.505;
}

typedef enum
{
    BFI_BRIGHT,
    BFI_DIM,
} BlackFramePhase;

typedef struct
{
    int runs;
    bool dim;
} BlackFrameStep;

static inline BlackFrameStep black_frames_step(BlackFramePhase *phase, bool active, int runs, bool speed_changed)
{
    if (!active)
    {
        *phase = BFI_BRIGHT;
        return (BlackFrameStep){runs, false};
    }
    bool dim = *phase == BFI_DIM;
    *phase = dim ? BFI_BRIGHT : BFI_DIM;
    /* Speed controls schedule emulation independently of BFI. */
    return (BlackFrameStep){speed_changed ? runs : dim ? 0 : 1, dim};
}

#endif
