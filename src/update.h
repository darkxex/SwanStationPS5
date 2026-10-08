/*
 * PSXS5 - updates from the project's GitHub releases.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSXS5_UPDATE_H
#define PSXS5_UPDATE_H

#include "psxs5.h"

enum UpdateState
{
    UPDATE_IDLE,
    UPDATE_CHECKING,
    UPDATE_NONE,      /* this is the newest version */
    UPDATE_AVAILABLE, /* update_version() is newer */
    UPDATE_INSTALLING,
    UPDATE_INSTALLED, /* restart PSXS5 to use it */
    UPDATE_FAILED,    /* update_message() says why */
};

#define UPDATE_REPO "darkxex/SwanStationPS5"

void update_check(void);   /* in the background */
void update_install(void); /* in the background; needs UPDATE_AVAILABLE */
int update_state(void);
const char *update_version(void);
const char *update_message(void);

#endif
