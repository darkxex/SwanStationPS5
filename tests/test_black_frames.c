/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "black_frames.h"

#include <assert.h>

int main(void)
{
    assert(black_frames_dim(0) == 255);
    assert(black_frames_dim(50) == 128);
    assert(black_frames_dim(100) == 0);
    assert(black_frames_dim(-1) == 255);
    assert(black_frames_dim(101) == 0);
    for (int percent = 1; percent <= 100; ++percent)
        assert(black_frames_dim(percent) <= black_frames_dim(percent - 1));
    assert(black_frames_active(true, true, 0, 119.88, 59.94));
    assert(black_frames_active(true, true, 0, 119.88, 59.817));
    assert(black_frames_active(true, true, 0, 120, 59.94));
    assert(!black_frames_active(false, true, 0, 120, 60));
    assert(!black_frames_active(true, false, 0, 120, 60));
    assert(!black_frames_active(true, true, 1, 120, 60));
    assert(!black_frames_active(true, true, 2, 120, 60));
    assert(!black_frames_active(true, true, 0, 60, 60));
    assert(!black_frames_active(true, true, 0, 120, 50));
    assert(!black_frames_active(true, true, 0, 0, 60));
    assert(!black_frames_active(true, true, 0, 120, 0));

    BlackFramePhase phase = BFI_BRIGHT;
    BlackFrameStep step;
    int frames = 0, dims = 0;
    for (int refresh = 0; refresh < 120; ++refresh)
    {
        step = black_frames_step(&phase, true, refresh % 3, false);
        assert(step.dim == (refresh % 2 != 0));
        assert(step.runs == (step.dim ? 0 : 1));
        frames += step.runs;
        dims += step.dim;
    }
    assert(frames == 60 && dims == 60);

    frames = dims = 0;
    for (int refresh = 0; refresh < 120; ++refresh)
    {
        step = black_frames_step(&phase, true, 4, true);
        assert(step.dim == (refresh % 2 != 0));
        assert(step.runs == 4);
        frames += step.runs;
        dims += step.dim;
    }
    assert(frames == 480 && dims == 60);

    /* Switch modes on both bright and dim refreshes without resetting the phase. */
    const struct
    {
        int requested;
        bool speed_changed;
        int bright_runs, dim_runs;
    } modes[] = {
        {2, false, 1, 0},
        {4, true, 4, 4},
        {0, true, 0, 0},
        {0, false, 1, 0},
        {0, true, 0, 0},
        {4, true, 4, 4},
        {1, false, 1, 0},
    };
    int refresh = 0;
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode)
        for (int i = 0; i < 5; ++i, ++refresh)
        {
            step = black_frames_step(&phase, true, modes[mode].requested, modes[mode].speed_changed);
            assert(step.dim == (refresh % 2 != 0));
            assert(step.runs == (refresh % 2 ? modes[mode].dim_runs : modes[mode].bright_runs));
        }

    /* Every phase restarts bright after disabling BFI. */
    for (int p = BFI_BRIGHT; p <= BFI_DIM; ++p)
        for (int runs = 0; runs <= 4; ++runs)
            for (int changed = 0; changed <= 1; ++changed)
            {
                phase = (BlackFramePhase)p;
                step = black_frames_step(&phase, false, runs, changed);
                assert(phase == BFI_BRIGHT && !step.dim && step.runs == runs);
                step = black_frames_step(&phase, true, runs, false);
                assert(step.runs == 1 && !step.dim);
            }
    return 0;
}
