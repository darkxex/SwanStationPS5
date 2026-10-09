/*
 * SwanStationPS5 - RetroAchievements password sign-in (shared by the console's own
 * sign-in screen and the phone page).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_RA_LOGIN_H
#define SwanStationPS5_RA_LOGIN_H

#include "../SwanStationPS5.h"

/* Exchanges the password for a login token (one HTTPS request to
 * retroachievements.org, blocking: call it from a thread). The password is not
 * kept anywhere. On success `who` is the account's name and `token` the token;
 * otherwise `error` says why (English, for the UI to show). */
bool ra_password_login(const char *name, const char *password, char *token, size_t token_size, char *who,
                       size_t who_size, char *error, size_t error_size);

#endif
