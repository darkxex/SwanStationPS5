/*
 * PSXS5 - cover art: local lookup, background download and decoding.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSXS5_COVERS_H
#define PSXS5_COVERS_H

#include "library.h"
#include "platform/platform.h"

/* (Re)binds the cover cache to a freshly scanned library. */
void covers_start(const Library *lib, const Paths *paths, const Settings *settings);
void covers_stop(void);

/* Main thread, once per frame: uploads finished images, requests the covers
 * around `center` and frees the ones far away. */
void covers_update(int center);
/* The same for a filtered or sorted shelf: view[k] is the library index at
 * shelf position k, center_pos the selected position. */
void covers_update_view(const int *view, int count, int center_pos);

/* Texture for game `index`; a generated title card until the art is ready. */
PlatTexture *covers_get(int index);
/* true when the picture is only a generated title card (no real cover found) */
bool covers_is_placeholder(int index);
/* Average colour of a loaded cover (0 until it has loaded). */
uint32_t covers_color(int index);

/* Downloads still queued, for the "Downloading covers" indicator. */
int covers_downloading(void);

/* URL of a cover in xlenore/psx-covers (also used by tools/psxs5_sync.py). */
void covers_url(char *out, size_t size, int style, const char *serial);

/* Forgets game `index`'s cover so it is looked up again (after a pick). */
void covers_reload(int index);
/* Where picked covers are kept: <root>/covers/custom/<game id>.<ext> */
void covers_custom_dir(char *out, size_t size);
/* .png / .jpg / .jpeg */
bool covers_is_image(const char *name);
/* Decodes an image to RGBA, scaled down like the shelf's covers; free() it. */
uint8_t *covers_decode(const char *path, int *w, int *h);

#endif
