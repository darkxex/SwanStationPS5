/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SwanStationPS5_STATE_SAVE_H
#define SwanStationPS5_STATE_SAVE_H

#include <stdbool.h>

#define THUMB_W 192
#define THUMB_H 144

/* Main-thread API. Only the immutable snapshot reaches the writer. */
void state_save_init(void);
/* Background saves skip if busy; foreground saves wait and report write success. */
bool state_save_request(const char *path, const char *directory, bool background);
void state_save_poll(void);
/* Outside gameplay: complete GPU readback and drain disk writes. */
void state_save_flush(void);
void state_save_shutdown(void);

#endif
