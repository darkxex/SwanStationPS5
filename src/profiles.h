/*
 * SwanStationPS5 - profiles: each person's memory cards, states, settings, play time
 * and RetroAchievements sign-in. Profile 0 is the main one (the data root).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_PROFILES_H
#define SwanStationPS5_PROFILES_H

#include "SwanStationPS5.h"

#define PROFILES_MAX 12
#define PROFILE_NAME_LEN 48

/* At start, before the settings load: points app.paths at the last profile. */
void profiles_startup(void);
int profiles_count(void);
int profiles_current(void);
const char *profiles_name(int i); /* "Main profile" for 0 */
int profiles_find(const char *name);
/* Adds one (its folder is made when it's first used); returns its index. */
int profiles_add(const char *name);
/* Takes it off the list; its files stay. Not the current one. */
bool profiles_remove(int i);
/* Saves this profile's things and loads that one's (only on the shelf). */
bool profiles_switch(int i);

#endif
