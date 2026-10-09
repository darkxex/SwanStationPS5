/*
 * SwanStationPS5 - profiles: several people on one console, each with their own
 * memory cards, save states, settings, play time and favourites, and
 * RetroAchievements sign-in. Games, BIOS, covers and cheats are shared.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The main profile is the data root itself (where everything lived before
 * profiles), so nothing moves. Others live in <root>/profiles/<name>/.
 * <root>/profiles.txt lists them (a sandboxed SwanStationPS5 can't list folders) and
 * <root>/profile.txt says whose turn it is.
 */
#include "profiles.h"

#include "app.h"
#include "config.h"
#include "i18n.h"
#include "ra/achievements.h"
#include "remote.h"
#include "stats.h"
#include "ui/coverflow.h"
#include "ui/sfx.h"
#include "ui/theme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char names[PROFILES_MAX][PROFILE_NAME_LEN]; /* names[0] is "" (the main profile) */
static int count = 1;
static int current;

static void list_path(char *out, size_t size)
{
    path_join(out, size, app.paths.root, "profiles.txt");
}

static void current_path(char *out, size_t size)
{
    path_join(out, size, app.paths.root, "profile.txt");
}

static void load_list(void)
{
    count = 1;
    names[0][0] = '\0';
    char path[SwanStationPS5_PATH_MAX];
    list_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f) && count < PROFILES_MAX)
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && line[0] != '#')
            str_copy(names[count++], PROFILE_NAME_LEN, line);
    }
    fclose(f);
}

static void save_list(void)
{
    char path[SwanStationPS5_PATH_MAX];
    list_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# SwanStationPS5 profiles (each has a folder in profiles/)\n");
    for (int i = 1; i < count; ++i)
        fprintf(f, "%s\n", names[i]);
    fclose(f);
}

/* The folder name: letters, digits, spaces, - and _ only. */
static void folder_of(const char *name, char *out, size_t size)
{
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && w + 1 < size; ++p)
        out[w++] = (isalnum(*p) || *p == ' ' || *p == '-' || *p == '_') ? (char)*p : '_';
    out[w] = '\0';
}

/* app.paths for profile i: the per-person folders under its own directory. */
static void apply_paths(int i)
{
    char dir[SwanStationPS5_PATH_MAX];
    if (i <= 0)
        str_copy(dir, sizeof(dir), app.paths.root);
    else
    {
        char sub[PROFILE_NAME_LEN + 16], folder[PROFILE_NAME_LEN];
        folder_of(names[i], folder, sizeof(folder));
        snprintf(sub, sizeof(sub), "profiles/%s", folder);
        path_join(dir, sizeof(dir), app.paths.root, sub);
    }
    config_user_paths(&app.paths, dir);
    make_dirs(app.paths.user);
    make_dirs(app.paths.saves);
    make_dirs(app.paths.states);
}

void profiles_startup(void)
{
    load_list();
    current = 0;
    char path[SwanStationPS5_PATH_MAX];
    current_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f)
    {
        char line[128] = "";
        if (fgets(line, sizeof(line), f))
        {
            line[strcspn(line, "\r\n")] = '\0';
            for (int i = 1; i < count; ++i)
                if (!strcmp(names[i], line))
                    current = i;
        }
        fclose(f);
    }
    apply_paths(current);
    if (current)
        SwanStationPS5_log("profile: %s (%s)", names[current], app.paths.user);
}

int profiles_count(void)
{
    return count;
}

int profiles_current(void)
{
    return current;
}

const char *profiles_name(int i)
{
    if (i < 0 || i >= count)
        return "";
    return i == 0 ? tr("Main profile") : names[i];
}

int profiles_find(const char *name)
{
    for (int i = 1; i < count; ++i)
        if (!strcmp(names[i], name))
            return i;
    return -1;
}

int profiles_add(const char *name)
{
    if (!name[0] || count >= PROFILES_MAX)
        return -1;
    int at = profiles_find(name);
    if (at >= 0)
        return at;
    str_copy(names[count], PROFILE_NAME_LEN, name);
    save_list();
    return count++;
}

bool profiles_remove(int i)
{
    if (i <= 0 || i >= count || i == current)
        return false;
    for (int k = i; k + 1 < count; ++k)
        memcpy(names[k], names[k + 1], PROFILE_NAME_LEN);
    --count;
    save_list(); /* its folder stays: nothing is deleted */
    return true;
}

bool profiles_switch(int i)
{
    if (i < 0 || i >= count || app.game)
        return false;
    if (i == current)
        return true;
    app_save_settings();
    stats_save();
    ra_shutdown();
    Settings before = app.global;
    current = i;
    apply_paths(i);
    FILE *f = fopen(app.paths.config, "r");
    if (f)
    {
        fclose(f);
        config_load(&app.global, app.paths.config);
    }
    else
    {
        /* a new profile starts with the settings in use */
        app.global = before;
        config_save(&app.global, app.paths.config);
    }
    app.settings = app.global;
    i18n_set(app.global.language);
    theme_apply(app.global.theme);
    sfx_configure(app.global.ui_sound, (app.global.ui_volume + 1) * 25);
    stats_load(app.paths.user);
    ra_init(&app.paths);
    char path[SwanStationPS5_PATH_MAX];
    current_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%s\n", i ? names[i] : "");
        fclose(f);
    }
    shelf_library_changed(); /* favourites and hidden games are the profile's */
    SwanStationPS5_log("profile: switched to %s", i ? names[i] : "the main profile");
    return true;
}
