/*
 * SwanStationPS5 - updates from the project's GitHub releases.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Checks api.github.com for the latest release; installing downloads its
 * SwanStationPS5-vX.Y.Z.zip and writes the PPSA98510 folder inside it over the
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
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_DIR_DEFAULT "/data/homebrew/PPSA98510"

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
    SwanStationPS5_log("update: %s", why);
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
                             SwanStationPS5_NAME "/" SwanStationPS5_VERSION, &body, &len);
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
    bool available = v[0] && newer(v, SwanStationPS5_VERSION) && zip_url[0];
    SwanStationPS5_log("update: latest %s, this %s%s", v[0] ? v : "?", SwanStationPS5_VERSION, available ? ", available" : "");
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

/* Where the running app is stored. The update has to land there, whether it's in /data/homebrew or on an
 * extended or USB drive (/mnt/ext1/homebrew/PPSA98510 ...). The mount the app runs from (/app0, or the
 * ShadowMountPlus one) is matched to a folder on disk by its eboot.bin: same size and modification time. */
static bool same_eboot(const char *dir, const struct stat *running)
{
    char file[SwanStationPS5_PATH_MAX];
    struct stat st;
    path_join(file, sizeof(file), dir, "eboot.bin");
    return stat(file, &st) == 0 && st.st_size == running->st_size && st.st_mtime == running->st_mtime;
}

static const char *app_dir(void)
{
    static char found[SwanStationPS5_PATH_MAX];
    static bool done;
    if (done)
        return found;
    done = true;
    str_copy(found, sizeof(found), APP_DIR_DEFAULT);
    static const char *const running_dirs[] = {"/app0", "/system_ex/app/" SwanStationPS5_TITLE_ID};
    static const char *const roots[] = {"/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1",
                                        "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"};
    struct stat running;
    bool have = false;
    for (size_t i = 0; i < sizeof(running_dirs) / sizeof(running_dirs[0]) && !have; ++i)
    {
        char file[SwanStationPS5_PATH_MAX];
        path_join(file, sizeof(file), running_dirs[i], "eboot.bin");
        have = stat(file, &running) == 0;
    }
    if (!have)
        return found;
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r)
    {
        char dir[SwanStationPS5_PATH_MAX];
        /* <root>/<id> and <root>/<any folder>/<id>, such as <root>/homebrew/<id> */
        path_join(dir, sizeof(dir), roots[r], SwanStationPS5_TITLE_ID);
        if (same_eboot(dir, &running))
        {
            str_copy(found, sizeof(found), dir);
            SwanStationPS5_log("update: the app is in %s", found);
            return found;
        }
        DIR *d = opendir(roots[r]);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] == '.')
                continue;
            char sub[SwanStationPS5_PATH_MAX];
            path_join(sub, sizeof(sub), roots[r], e->d_name);
            path_join(dir, sizeof(dir), sub, SwanStationPS5_TITLE_ID);
            if (same_eboot(dir, &running))
            {
                str_copy(found, sizeof(found), dir);
                closedir(d);
                SwanStationPS5_log("update: the app is in %s", found);
                return found;
            }
        }
        closedir(d);
    }
    SwanStationPS5_log("update: couldn't find where the app is stored, using %s", found);
    return found;
}

static int install_thread(void *unused)
{
    (void)unused;
    char zip[SwanStationPS5_PATH_MAX];
    path_join(zip, sizeof(zip), app.paths.root, "cache/update.zip");
    char dir[SwanStationPS5_PATH_MAX];
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
        /* SwanStationPS5-v1.2.0/PPSA98510/<path> -> <where the app is stored>/<path> */
        const char *inside = strstr(st.m_filename, "PPSA98510/");
        if (!inside || strstr(inside, ".."))
            continue;
        inside += strlen("PPSA98510/");
        char target[SwanStationPS5_PATH_MAX], temp[SwanStationPS5_PATH_MAX + 8], parent[SwanStationPS5_PATH_MAX];
        path_join(target, sizeof(target), app_dir(), inside);
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
    SwanStationPS5_log("update: installed %s (%d files)", version, files);
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
