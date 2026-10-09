/*
 * SwanStationPS5 - things around the running game: quick resume, state thumbnails,
 * rewind, fast forward, widescreen codes and fan-translation patches.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_PLAY_H
#define SwanStationPS5_PLAY_H

#include "library.h"

/* Before host_load: points the core at the game's .ppf patch, if it has one.
 * Returns true when a translation patch will be applied. */
bool play_prepare_patch(const Game *g);
/* After cheats are loaded: switches on the game's widescreen code (if
 * Settings > Display > Widescreen is on). True when the game has one. */
bool play_widescreen(void);
bool play_widescreen_active(void);

/* Quick resume: <states>/<id>.resume, written when you quit (and every few
 * minutes in the background), offered as Continue on the shelf. */
void play_resume_path(const Game *g, char *out, size_t size);
bool play_has_resume(const Game *g, long *age_seconds);
void play_save_resume(bool background);
bool play_load_resume(void);

/* Auto-saves (Settings > System > Auto-save): <states>/<id>.auto1..3, the
 * oldest replaced each time, written in the background. */
#define AUTO_SLOTS 3
void play_auto_path(int slot, char *out, size_t size);
bool play_save_auto(void);
/* The auto-saves there are, newest first; returns how many. */
int play_auto_list(int slots[AUTO_SLOTS], long ages[AUTO_SLOTS]);

/* State thumbnails: <state file>.thumb, a small picture of the moment. */
#define THUMB_W 192
#define THUMB_H 144
void play_save_thumb(const char *state_path);
/* The game picture as a PNG in <root>/screenshots, at the size it has on screen. */
bool play_screenshot(void);
/* Loads it into rgba (THUMB_W x THUMB_H); false if there is none. */
bool play_load_thumb(const char *state_path, uint8_t *rgba);

/* Rewind: call after every emulated frame; rewinding steps back instead. */
void play_rewind_record(void);
bool play_rewind_step(void); /* false when there is nothing left to go back to */
void play_rewind_reset(void);

#endif
