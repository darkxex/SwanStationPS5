/*
 * PSXS5 - updates from the project's GitHub releases.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Checks api.github.com for the latest release; installing downloads its
 * PSXS5-vX.Y.Z.zip and writes the PPSA98510 folder inside it over the
 * installed app. Each file goes to <name>.new and is renamed into place:
 * a running eboot.bin can't be overwritten, but it can be replaced by name,
 * and the new one runs from the next start.
 */
#include "update.h"

#include "app.h"
#include "i18n.h"
#include "net.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_DIR "/data/homebrew/PPSA98510"

static SDL_atomic_t state;
static char version[32], zip_url[512], message[256];

int update_state(void)
{
    return SDL_AtomicGet(&state);
}

const char *update_version(void)
{
    return version;
}

const char *update_message(void)
{
    return message;
}

/* "1.10.0" > "1.9.2" */
static bool newer(const char *a, const char *b)
{
    int x[3] = {0}, y[3] = {0};
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i])
            return x[i] > y[i];
    return false;
}

/* the value of "key": "..." after `from` in a JSON text */
static bool json_string(const char *from, const char *key, char *out, size_t size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(from, pattern);
    if (!p)
        return false;
    p = strchr(p + strlen(pattern), '"');
    if (!p)
        return false;
    const char *end = strchr(++p, '"');
    if (!end || (size_t)(end - p) >= size)
        return false;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return true;
}

static void fail(const char *why)
{
    str_copy(message, sizeof(message), why);
    psxs5_log("update: %s", why);
    SDL_AtomicSet(&state, UPDATE_FAILED);
}

static int check_thread(void *unused)
{
    (void)unused;
    char *body = NULL;
    size_t len = 0;
    /* every recent release, not /releases/latest: from 2.1.1 on, new releases
     * aren't marked Latest, so that 2.0.0 (whose updater left the app unable to
     * start) never offers them */
    int status = net_request("https://api.github.com/repos/" UPDATE_REPO "/releases?per_page=15", NULL, NULL,
                             PSXS5_NAME "/" PSXS5_VERSION, &body, &len);
    if (status == 404)
    {
        free(body);
        fail(tr("No release found (the repository may be private)"));
        return 0;
    }
    if (status != 200 || !body)
    {
        free(body);
        fail(tr("Couldn't reach GitHub"));
        return 0;
    }
    /* the newest release (not a draft or a pre-release) that has a .zip */
    version[0] = '\0';
    zip_url[0] = '\0';
    for (const char *rel = strstr(body, "\"tag_name\""); rel;)
    {
        const char *next = strstr(rel + 10, "\"tag_name\"");
        size_t span = next ? (size_t)(next - rel) : strlen(rel);
        char *one = malloc(span + 1);
        if (!one)
            break;
        memcpy(one, rel, span);
        one[span] = '\0';
        char tag[32] = "", url[512] = "", found[512] = "";
        json_string(one, "tag_name", tag, sizeof(tag));
        bool skip = strstr(one, "\"draft\":true") || strstr(one, "\"prerelease\":true");
        for (const char *p = one; !skip && (p = strstr(p, "\"browser_download_url\""));)
        {
            if (json_string(p, "browser_download_url", url, sizeof(url)) && strlen(url) > 4 &&
                !strcmp(url + strlen(url) - 4, ".zip"))
            {
                str_copy(found, sizeof(found), url);
                break;
            }
            p += 22;
        }
        free(one);
        const char *tv = tag[0] == 'v' ? tag + 1 : tag;
        if (!skip && tv[0] && found[0] && (!version[0] || newer(tv, version)))
        {
            str_copy(version, sizeof(version), tv);
            str_copy(zip_url, sizeof(zip_url), found);
        }
        rel = next;
    }
    free(body);
    const char *v = version;
    bool available = v[0] && newer(v, PSXS5_VERSION) && zip_url[0];
    psxs5_log("update: latest %s, this %s%s", v[0] ? v : "?", PSXS5_VERSION, available ? ", available" : "");
    SDL_AtomicSet(&state, available ? UPDATE_AVAILABLE : UPDATE_NONE);
    return 0;
}

void update_check(void)
{
    int s = update_state();
    if (s == UPDATE_CHECKING || s == UPDATE_INSTALLING || s == UPDATE_INSTALLED)
        return;
    SDL_AtomicSet(&state, UPDATE_CHECKING);
    SDL_Thread *t = SDL_CreateThread(check_thread, "update", NULL);
    if (t)
        SDL_DetachThread(t);
    else
        fail("no thread");
}

static int install_thread(void *unused)
{
    (void)unused;
    char zip[PSXS5_PATH_MAX];
    path_join(zip, sizeof(zip), app.paths.root, "cache/update.zip");
    char dir[PSXS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "cache");
    make_dirs(dir);
    if (net_download(zip_url, zip) != NET_OK)
    {
        fail(tr("The download failed"));
        return 0;
    }
    mz_zip_archive archive;
    memset(&archive, 0, sizeof(archive));
    if (!mz_zip_reader_init_file(&archive, zip, 0))
    {
        fail(tr("The download isn't a valid zip"));
        return 0;
    }
    int files = 0;
    bool ok = true;
    mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count && ok; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st) || st.m_is_directory)
            continue;
        /* PSXS5-v1.2.0/PPSA98510/<path> -> APP_DIR/<path> */
        const char *inside = strstr(st.m_filename, "PPSA98510/");
        if (!inside || strstr(inside, ".."))
            continue;
        inside += strlen("PPSA98510/");
        char target[PSXS5_PATH_MAX], temp[PSXS5_PATH_MAX + 8], parent[PSXS5_PATH_MAX];
        path_join(target, sizeof(target), APP_DIR, inside);
        str_copy(parent, sizeof(parent), target);
        char *slash = strrchr(parent, '/');
        if (slash)
        {
            *slash = '\0';
            make_dirs(parent);
        }
        snprintf(temp, sizeof(temp), "%s.new", target);
        /* executable, as an FTP upload makes them: without it the PS5 won't start
         * the app ("Can't start the game or app"), which 2.0.0's updater did */
        ok = mz_zip_reader_extract_to_file(&archive, i, temp, 0) && chmod(temp, 0777) == 0 &&
             rename(temp, target) == 0;
        if (!ok)
            remove(temp);
        files += ok;
    }
    mz_zip_reader_end(&archive);
    remove(zip);
    if (!ok || files == 0)
    {
        fail(tr("Couldn't write the new files (is /data unlocked?)"));
        return 0;
    }
    snprintf(message, sizeof(message), tr("SwanStationPS5 %s is installed: restart SwanStationPS5 to use it"), version);
    psxs5_log("update: installed %s (%d files)", version, files);
    SDL_AtomicSet(&state, UPDATE_INSTALLED);
    return 0;
}

void update_install(void)
{
    if (update_state() != UPDATE_AVAILABLE)
        return;
    SDL_AtomicSet(&state, UPDATE_INSTALLING);
    SDL_Thread *t = SDL_CreateThread(install_thread, "update", NULL);
    if (t)
        SDL_DetachThread(t);
    else
        fail("no thread");
}
