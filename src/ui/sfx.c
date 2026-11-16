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
#include <stdio.h>
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

static Sound disc_sound, notification_sound;
static int sound_rate = 44100;
#define DISC_GAIN 0.5f /* the disc sound's gain, before the interface's volume */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A PCM WAV of 16 bits, mono or stereo, at any rate: stereo at `rate`. */
static Sound load_wav(const char *path, int rate)
{
    Sound s = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return s;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = size > 12 && size < (16 << 20) ? malloc((size_t)size) : NULL;
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size)
    {
        free(d);
        fclose(f);
        return s;
    }
    fclose(f);
    int channels = 0, bits = 0;
    uint32_t src_rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_bytes = 0;
    if (memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0)
        for (size_t i = 12; i + 8 <= (size_t)size;)
        {
            const uint32_t n = le32(d + i + 4);
            if (!memcmp(d + i, "fmt ", 4) && n >= 16 && i + 8 + 16 <= (size_t)size && d[i + 8] == 1)
            {
                channels = d[i + 10];
                src_rate = le32(d + i + 12);
                bits = d[i + 22];
            }
            else if (!memcmp(d + i, "data", 4) && i + 8 + n <= (size_t)size)
            {
                data = d + i + 8;
                data_bytes = n;
                break;
            }
            i += 8 + (size_t)n + (n & 1);
        }
    if (data && bits == 16 && (channels == 1 || channels == 2) && src_rate > 0 && rate > 0)
    {
        const size_t in_frames = data_bytes / (size_t)(2 * channels);
        const size_t out_frames = (size_t)((double)in_frames * rate / src_rate);
        s.frames = in_frames > 1 ? malloc(out_frames * 2 * sizeof(int16_t)) : NULL;
        for (size_t k = 0; s.frames && k < out_frames; ++k)
        {
            const double pos = (double)k * src_rate / rate;
            size_t a = (size_t)pos, b = a + 1 < in_frames ? a + 1 : a;
            const double t = pos - (double)a;
            for (int c = 0; c < 2; ++c)
            {
                const int ch = channels == 1 ? 0 : c;
                const int16_t x = (int16_t)(data[(a * channels + ch) * 2] | data[(a * channels + ch) * 2 + 1] << 8);
                const int16_t y = (int16_t)(data[(b * channels + ch) * 2] | data[(b * channels + ch) * 2 + 1] << 8);
                s.frames[k * 2 + c] = (int16_t)((double)x + ((double)y - (double)x) * t);
            }
        }
        s.count = s.frames ? out_frames : 0;
    }
    free(d);
    return s;
}

void sfx_init(int rate)
{
    sound_rate = rate > 0 ? rate : 44100;
    free(disc_sound.frames);
    free(notification_sound.frames);
    char path[512];
    plat_asset_path(path, sizeof(path), "psxdiscsound.wav");
    disc_sound = load_wav(path, rate);
    plat_asset_path(path, sizeof(path), "notification.wav");
    notification_sound = load_wav(path, rate);
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

static bool silenced;

void sfx_silence(bool on)
{
    silenced = on;
}

#define VOICES 8

typedef struct
{
    const Sound *sound;
    size_t pos;
    float gain;
} Voice;

static Voice voices[VOICES];
static bool game_audio;

static void voice_start(const Sound *s, float gain)
{
    Voice *slot = &voices[0];
    for (int i = 0; i < VOICES; ++i)
    {
        if (!voices[i].sound)
        {
            slot = &voices[i];
            break;
        }
        if (voices[i].pos > slot->pos)
            slot = &voices[i];
    }
    slot->sound = s;
    slot->pos = 0;
    slot->gain = gain;
}

void sfx_mix(int16_t *frames, size_t frame_count)
{
    for (int v = 0; v < VOICES; ++v)
    {
        Voice *p = &voices[v];
        if (!p->sound)
            continue;
        const float g = p->gain * volume;
        size_t i = 0;
        for (; i < frame_count && p->pos < p->sound->count; ++i, ++p->pos)
            for (int c = 0; c < 2; ++c)
            {
                const int x = frames[i * 2 + c] + (int)(p->sound->frames[p->pos * 2 + c] * g);
                frames[i * 2 + c] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            }
        if (p->pos >= p->sound->count)
            p->sound = NULL;
    }
}

void sfx_game_audio(bool on)
{
    game_audio = on;
}

void sfx_pump(void)
{
    if (game_audio)
        return;
    bool any = false;
    for (int i = 0; i < VOICES; ++i)
        any |= voices[i].sound != NULL;
    if (!any)
        return;
    const size_t target = (size_t)(sound_rate / 20), queued = plat_audio_queued_frames();
    if (queued >= target)
        return;
    static int16_t block[2 * 4096];
    size_t n = target - queued;
    n = n > 4096 ? 4096 : n;
    memset(block, 0, n * 2 * sizeof(int16_t));
    sfx_mix(block, n);
    plat_audio_push(block, n);
}

void sfx_play(Sfx id)
{
    if (silenced || !enabled || id >= SFX_COUNT || !sounds[style][id].frames || volume <= 0.0f)
        return;
    voice_start(&sounds[style][id], 1.0f);
}

void sfx_play_notification(void)
{
    if (silenced || !notification_sound.frames || volume <= 0.0f)
        return;
    voice_start(&notification_sound, 1.0f);
}

void sfx_play_disc(void)
{
    if (silenced || !enabled || !disc_sound.frames || volume <= 0.0f)
        return;
    voice_start(&disc_sound, DISC_GAIN);
}
