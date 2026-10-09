/*
 * SwanStationPS5 - asking the HEN to let SwanStationPS5 list /data.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_PS5_UNLOCK_H
#define SwanStationPS5_PS5_UNLOCK_H

#include <stdbool.h>

typedef enum
{
    UNLOCK_OK,
    UNLOCK_ALREADY,
    UNLOCK_NO_ANSWER,
    UNLOCK_CANT_REQUEST,
    UNLOCK_SKIPPED_AFTER_CRASH,
} UnlockResult;

bool ps5_data_listable(void);
/* Call first thing in main(), before any thread exists. */
UnlockResult ps5_unlock_etahen(void);
void ps5_unlock_reset_crash_marker(void);
const char *ps5_unlock_describe(UnlockResult result);

#endif
