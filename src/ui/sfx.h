/*
 * SwanStationPS5 - interface sounds, synthesised at start-up (no sample files).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_SFX_H
#define SwanStationPS5_SFX_H

#include "../SwanStationPS5.h"

typedef enum
{
    SFX_CLICK,  /* moving along the shelf or a list */
    SFX_SELECT, /* opening a game or a panel */
    SFX_BACK,
    SFX_COUNT
} Sfx;

/* Settings > Interface sound. SFX_STYLE_OFF must stay last. */
enum
{
    SFX_STYLE_SOFT = 0,
    SFX_STYLE_WOOD,
    SFX_STYLE_POP,
    SFX_STYLE_CHIME,
    SFX_STYLE_CLASSIC,
    SFX_STYLE_COUNT,
    SFX_STYLE_OFF = SFX_STYLE_COUNT,
};

void sfx_init(int sample_rate);
void sfx_play(Sfx sound);
/* assets/psxdiscsound.wav (the CD spinning up in the 3D case), with the interface's volume; nothing when the interface
 * sound is off or the file is not there. */
void sfx_play_disc(void);
/* assets/notification.wav, with the interface's volume (also when the interface sound is off); nothing when the file
 * is not there. */
void sfx_play_notification(void);
/* The sounds play in parallel, each one on its own and mixed into what is playing. While a game runs, they are added to
 * the game's sound by sfx_mix (the host calls it on every block); otherwise sfx_pump, once a frame, plays them. */
void sfx_mix(int16_t *frames, size_t frame_count);
void sfx_game_audio(bool on);
void sfx_pump(void);
/* While on, the app plays no sounds (Nostalgia mode during a game). */
void sfx_silence(bool on);
/* style: SFX_STYLE_*, or SFX_STYLE_OFF; volume 0..100 */
void sfx_configure(int style, int volume_percent);
void sfx_set_enabled(bool enabled); /* temporary mute, keeps the style */

#endif
