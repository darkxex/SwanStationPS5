/*
 * SwanStationPS5 - minimal HTTPS download (libcurl).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_NET_H
#define SwanStationPS5_NET_H

#include "SwanStationPS5.h"

typedef enum
{
    NET_OK,
    NET_NOT_FOUND,   /* the server answered 404: the file does not exist */
    NET_UNAVAILABLE, /* no network, DNS, TLS... worth giving up for this session */
} NetResult;

bool net_available(void); /* false when built without libcurl */
/* Downloads `url` to `dest` (written to dest.part, then renamed). Blocking. */
NetResult net_download(const char *url, const char *dest);

/* GET (post_data NULL) or POST `url`; the body comes back in *body (malloc'd,
 * NUL-terminated, free() it). Returns the HTTP status, or a negative value
 * when the request could not be made at all. Blocking. */
int net_request(const char *url, const char *post_data, const char *content_type,
                const char *user_agent, char **body, size_t *body_length);

#endif
