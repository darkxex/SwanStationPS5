/*
 * SwanStationPS5 - interface sounds, synthesised at start-up (no sample files).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each sound is a short band-passed noise burst (the "tick") over a pitched
 * sine body, shaped by an exponential decay. Styles are just different
 * recipes; the default "Soft" is a quiet low tock.
 */
#include "sfx.h"

#include "../platform/platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    int16_t *frames; /* interleaved stereo */
    size_t count;
} Sound;

typedef struct
{
    float seconds;
    float tick_freq, tick_q, tick_gain; /* noise burst */
    float body_from, body_to, body_gain; /* pitched sine */
    float overtone;                      /* gain of a 2x harmonic (chime) */
    float decay;                         /* 1/s */
} Recipe;

/* SFX_CLICK per style: Soft, Wood, Pop, Chime, Classic */
static const Recipe CLICKS[SFX_STYLE_COUNT] = {
    {0.060f, 1200.0f, 0.9f, 0.05f, 140.0f, 90.0f, 0.22f, 0.0f, 70.0f},   /* Soft */
    {0.050f, 900.0f, 4.0f, 0.22f, 240.0f, 200.0f, 0.14f, 0.0f, 95.0f},   /* Wood */
    {0.045f, 2000.0f, 1.0f, 0.00f, 900.0f, 320.0f, 0.20f, 0.0f, 85.0f},  /* Pop */
    {0.110f, 3000.0f, 1.0f, 0.00f, 1320.0f, 1320.0f, 0.10f, 0.45f, 38.0f}, /* Chime */
    {0.045f, 2600.0f, 1.6f, 0.45f, 190.0f, 70.0f, 0.28f, 0.0f, 120.0f},  /* Classic */
};
/* SFX_SELECT and SFX_BACK: one gentle pair for every style */
static const Recipe SELECT = {0.14f, 1800.0f, 1.2f, 0.08f, 330.0f, 520.0f, 0.18f, 0.25f, 28.0f};
static const Recipe BACK = {0.09f, 1400.0f, 1.2f, 0.06f, 360.0f, 220.0f, 0.16f, 0.0f, 45.0f};

static Sound sounds[SFX_STYLE_COUNT][SFX_COUNT];
static int style = SFX_STYLE_SOFT;
static float volume = 0.5f;
static bool enabled = true;

static uint32_t rng = 0x12345678u;
static float noise(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 8388608.0f - 1.0f;
}

typedef struct
{
    float a1, a2, b0, y1, y2;
} Resonator;

static Resonator resonator(float freq, float q, int rate)
{
    float w = 2.0f * 3.14159265f * freq / rate, r = expf(-w / (2.0f * q));
    Resonator f = {2.0f * r * cosf(w), -r * r, (1.0f - r * r) * 0.5f, 0, 0};
    return f;
}

static float resonate(Resonator *f, float x)
{
    float y = f->b0 * x + f->a1 * f->y1 + f->a2 * f->y2;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

static Sound make(int rate, const Recipe *r)
{
    Sound s;
    s.count = (size_t)(rate * r->seconds);
    s.frames = calloc(s.count * 2, sizeof(int16_t));
    if (!s.frames)
    {
        s.count = 0;
        return s;
    }
    Resonator tick = resonator(r->tick_freq, r->tick_q, rate);
    float phase = 0.0f;
    for (size_t i = 0; i < s.count; ++i)
    {
        float t = (float)i / rate;
        float env = expf(-t * r->decay);
        float freq = r->body_from * powf(r->body_to / r->body_from, fminf(t / r->seconds, 1.0f));
        phase += 2.0f * 3.14159265f * freq / rate;
        float v = resonate(&tick, noise()) * 6.0f * r->tick_gain * expf(-t * r->decay * 1.8f) +
                  (sinf(phase) + r->overtone * sinf(2.0f * phase)) * r->body_gain * env;
        /* 3 ms fade-in and a fade-out over the last 20% avoid clicks of their own */
        float attack = fminf(t / 0.003f, 1.0f);
        float release = fminf((r->seconds - t) / (r->seconds * 0.2f), 1.0f);
        int sample = (int)(v * attack * release * 32767.0f);
        sample = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
        s.frames[i * 2] = s.frames[i * 2 + 1] = (int16_t)sample;
    }
    return s;
}

void sfx_init(int rate)
{
    for (int st = 0; st < SFX_STYLE_COUNT; ++st)
    {
        for (int i = 0; i < SFX_COUNT; ++i)
            free(sounds[st][i].frames);
        sounds[st][SFX_CLICK] = make(rate, &CLICKS[st]);
        sounds[st][SFX_SELECT] = make(rate, &SELECT);
        sounds[st][SFX_BACK] = make(rate, &BACK);
    }
}

void sfx_configure(int new_style, int volume_percent)
{
    enabled = new_style != SFX_STYLE_OFF;
    if (enabled && new_style >= 0 && new_style < SFX_STYLE_COUNT)
        style = new_style;
    volume = (float)(volume_percent < 0 ? 0 : volume_percent > 100 ? 100 : volume_percent) / 100.0f;
}

void sfx_set_enabled(bool on)
{
    enabled = on;
}

void sfx_play(Sfx id)
{
    if (!enabled || id >= SFX_COUNT || !sounds[style][id].frames || volume <= 0.0f)
        return;
    const Sound *s = &sounds[style][id];
    /* Fast scrolling must not build a backlog of clicks. */
    if (plat_audio_queued_frames() > s->count)
        plat_audio_clear();
    static int16_t scaled[2 * 48000 / 4]; /* up to 250 ms of stereo at 48 kHz */
    size_t n = s->count * 2 < sizeof(scaled) / sizeof(scaled[0]) ? s->count * 2
                                                                  : sizeof(scaled) / sizeof(scaled[0]);
    for (size_t i = 0; i < n; ++i)
        scaled[i] = (int16_t)(s->frames[i] * volume);
    plat_audio_push(scaled, n / 2);
}
