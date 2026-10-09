/*
 * SwanStationPS5 - what SwanStationPS5 does with the controller while a game runs: stick dead
 * zone and response, gas and brake on the triggers in racing games, the
 * DualSense as a light gun, and the adaptive triggers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_CONTROLS_H
#define SwanStationPS5_CONTROLS_H

#include "library.h"
#include "SwanStationPS5.h"

enum GameKind
{
    KIND_GUNCON = 1,
    KIND_JUSTIFIER = 2,
    KIND_RACING = 4, /* Cross gas, Square brake: our list */
    KIND_PEDAL = 8,  /* a racing game (the database): only the trigger feel */
};

/* KIND_* bits for a game, from assets/game-kinds.txt. */
int controls_kind(const Game *g);
/* The light gun a game gets with these settings: 0 none, 1 GunCon, 2 Justifier. */
int controls_gun_for(const Game *g, const Settings *s);
/* Other controllers: 0 pads, 1 NeGcon (racing games that take one), 2 the
 * touchpad as a mouse (mouse games); from DuckStation's database. */
int controls_special_for(const Game *g, const Settings *s);
/* Before the game loads, and when it stops (the triggers go back to normal). */
void controls_start(const Game *g, const Settings *s);
void controls_stop(void);
/* Each frame, on the buttons the game will see. */
void controls_apply(PadState pads[SwanStationPS5_MAX_PADS], const Settings *s, float dt);
/* The gun's crosshair, over the game. */
void controls_draw(void);
bool controls_gun_active(void);
void controls_recenter(void);

#endif
