/*
 * PSXS5 - RetroAchievements password sign-in.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "login.h"

#include "../net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void url_encode(char *out, size_t size, const char *s)
{
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && w + 4 < size; ++p)
    {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p))
            out[w++] = (char)*p;
        else
            w += (size_t)snprintf(out + w, size - w, "%%%02X", *p);
    }
    out[w] = '\0';
}

/* the value of "key":"..." in a JSON reply (no escapes in what we read) */
static bool json_value(const char *json, const char *key, char *out, size_t size)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return false;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= size)
        return false;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return true;
}

bool ra_password_login(const char *name, const char *password, char *token, size_t token_size, char *who,
                       size_t who_size, char *error, size_t error_size)
{
    char post[800], enc_user[200], enc_pass[800];
    url_encode(enc_user, sizeof(enc_user), name);
    url_encode(enc_pass, sizeof(enc_pass), password);
    snprintf(post, sizeof(post), "r=login2&u=%s&p=%s", enc_user, enc_pass);
    memset(enc_pass, 0, sizeof(enc_pass));
    char *reply = NULL;
    size_t reply_len = 0;
    int status = net_request("https://retroachievements.org/dorequest.php", post,
                             "application/x-www-form-urlencoded", PSXS5_NAME "/" PSXS5_VERSION, &reply, &reply_len);
    memset(post, 0, sizeof(post));
    error[0] = '\0';
    bool ok = reply && strstr(reply, "\"Success\":true") && json_value(reply, "Token", token, token_size);
    if (ok)
    {
        if (!json_value(reply, "User", who, who_size))
            str_copy(who, who_size, name);
    }
    else if (reply && json_value(reply, "Error", error, error_size))
        ;
    else
        snprintf(error, error_size, status < 0 ? "Couldn't reach RetroAchievements" : "Sign-in failed (%d)", status);
    free(reply);
    return ok;
}
