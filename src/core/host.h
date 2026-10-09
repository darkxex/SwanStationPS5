/*
 * SwanStationPS5 - libretro host for the statically linked SwanStation core.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_HOST_H
#define SwanStationPS5_HOST_H

#include "../SwanStationPS5.h"

/* serial: the disc's (SLUS-00662), for the emulator choice and the memory
 * card; may be empty. */
bool host_load(const char *game_path, const char *serial, const Paths *paths, const Settings *settings,
               char *error, size_t error_size);
void host_unload(void);
bool host_loaded(void);
/* "SwanStation": the emulator of the loaded game */
const char *host_core_name(void);
/* The emulator a game would get with these settings ("SwanStation"); *why_not_beetle is always NULL. */
const char *host_emulator_for(const Settings *settings, const char *serial, const char **why_not_beetle);

void host_set_pads(const PadState pads[SwanStationPS5_MAX_PADS]);
/* Port 1's device for the next host_load: 0 a pad, 1 GunCon, 2 Justifier. */
void host_set_gun(int device);
/* Other controllers for the next host_load: 0 pads, 1 NeGcon in every port, 2 a mouse in port 1. */
void host_set_special(int device);
/* Known fixes for the next host_load (GDB_* from gamedb.h): settings the game breaks with. */
void host_set_fixes(unsigned flags);
/* How hard the game is rumbling this player's controller now, 0..1. */
float host_rumble_level(int port);
void host_run_frame(void);
/* Run-ahead: frames run only to show the future are silent and don't rumble. */
void host_set_speculative(bool on);
uint32_t host_frames_new(void);      /* the core's frames so far: the game's own... */
uint32_t host_frames_repeated(void); /* ...and the ones that repeat the last (a 30 fps game on a 60 fps core) */
void host_reset(void);
void host_apply_settings(const Settings *settings); /* takes effect without reloading */

double host_fps(void);
int host_sample_rate(void);
float host_aspect(void);
/* Latest frame; returns NULL when the core has not produced one yet. */
const void *host_frame(int *width, int *height, size_t *pitch, int *pixel_format,
                       bool *fresh);

bool host_save_state(const char *path);
bool host_load_state(const char *path);
/* Save states in memory (rewind, quick resume). */
size_t host_state_size(void);
bool host_serialize(void *buffer, size_t size);
bool host_unserialize(const void *buffer, size_t size);
/* The latest frame scaled to w x h RGBA (save-state thumbnails). */
bool host_capture(uint8_t *rgba, int w, int h);
/* Folder the core reads fan-translation patches from (before host_load). */
void host_set_patches_dir(const char *dir);

/* For RetroAchievements */
struct retro_memory_map;
const struct retro_memory_map *host_memory_map(void);
void *host_memory_data(unsigned id); /* retro_get_memory_data of the running core */
size_t host_memory_size(unsigned id);

int host_disc_count(void);
int host_disc_index(void);
bool host_disc_select(int index);

/* GameShark codes, to the running core */
void host_cheat_reset(void);
void host_cheat_set(unsigned index, const char *code);
/* Beetle PSX HW: its renderer's widescreen mode (no-op for other cores). */
void host_beetle_widescreen(bool on);

#endif
