/*
 * SwanStationPS5 - settings from a phone: a small web page on the local network.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One thread answers HTTP on REMOTE_PORT:
 *   GET /               assets/remote.html
 *   GET /api/settings   every setting as JSON (a snapshot the main thread keeps fresh)
 *   POST /api/set?key=K&value=V
 *   GET /api/saves      every game's memory card and save-state slots (+ notes)
 *   GET|POST /api/save?game=ID&type=card|state&slot=N   download / upload
 *   GET /api/thumb?game=ID&slot=N   a slot's picture as PNG
 *   POST /api/note?game=ID&slot=N   a slot's note (the body, one line)
 *   POST /api/ra-login  user=U&password=P: signs in to RetroAchievements; the
 *                       password goes to retroachievements.org and nowhere else
 *   POST /api/ra-logout
 * Changes are queued and applied by the main thread (remote_frame), so the
 * settings are never touched from two threads.
 */
#include "remote.h"

#include "app.h"
#include "ra/achievements.h"
#include "ra/login.h"
#include "net.h"
#include "i18n.h"
#include "platform/platform.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* provided by ui/settings_screen.c */
int settings_json(char *out, size_t size);
const char *settings_set_by_key(const char *key, int value);

#if defined(_WIN32)

/* the PC preview has no server */
void remote_update(bool enabled) { (void)enabled; }
void remote_frame(void) {}
const char *remote_address(void) { return ""; }

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SNAPSHOT_SIZE (96 * 1024)
#define PENDING 16

static SDL_Thread *thread;
static SDL_mutex *lock;
static SDL_atomic_t quit;
static int listen_fd = -1;
static char address[64];
static char *snapshot;
static size_t snapshot_len;
static struct
{
    char key[16];
    int value;
} pending[PENDING];
static int pending_count;

static void send_all(int fd, const char *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = send(fd, data, len, 0);
        if (n <= 0)
            return;
        data += n;
        len -= (size_t)n;
    }
}

static void respond(int fd, const char *status, const char *type, const char *body, size_t len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static char *read_asset(const char *name, size_t *len)
{
    char path[SwanStationPS5_PATH_MAX];
    plat_asset_path(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = data ? (size_t)size : 0;
    return data;
}

static const char *query(const char *q, const char *name, char *out, size_t size)
{
    size_t n = strlen(name);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
        if (!strncmp(p, name, n) && p[n] == '=')
        {
            size_t len = strcspn(p + n + 1, "& \r\n");
            if (len >= size)
                len = size - 1;
            memcpy(out, p + n + 1, len);
            out[len] = '\0';
            return out;
        }
    return NULL;
}

static void reply_json(int fd, const char *status, const char *json)
{
    respond(fd, status, "application/json", json, strlen(json));
}

/* ---------------------------------------------------------------- saves from the phone */

/* The library as the phone sees it: id, title and serial of every game,
 * copied by the main thread (remote_frame) so this thread never reads the
 * Library while it changes. */
#define REMOTE_GAMES 4096
typedef struct
{
    char id[64], title[96], serial[16];
} RemoteGame;
static RemoteGame *games;
static int game_count;
static char running_id[64]; /* the game being played, "" on the shelf */

static const RemoteGame *find_game(const char *id)
{
    for (int i = 0; i < game_count; ++i)
        if (!strcmp(games[i].id, id))
            return &games[i];
    return NULL;
}

static void state_file(const RemoteGame *g, int slot, char *out, size_t size)
{
    char file[96];
    snprintf(file, sizeof(file), "%.80s.state%d", g->id, slot);
    path_join(out, size, app.paths.states, file);
}

/* The card: <saves>/<serial>_1.mcd (lower case as some
 * discs spell it). "" when the game has no serial. */
static void card_file(const RemoteGame *g, char *out, size_t size)
{
    out[0] = '\0';
    if (!g->serial[0])
        return;
    char name[48];
    snprintf(name, sizeof(name), "%s_1.mcd", g->serial);
    path_join(out, size, app.paths.saves, name);
    FILE *f = fopen(out, "rb");
    if (f)
    {
        fclose(f);
        return;
    }
    for (char *c = name; *c; ++c)
        *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    char lower[SwanStationPS5_PATH_MAX];
    path_join(lower, sizeof(lower), app.paths.saves, name);
    if ((f = fopen(lower, "rb")) != NULL)
    {
        fclose(f);
        str_copy(out, size, lower);
    }
}

static long long file_time(const char *path, long *bytes)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (bytes)
        *bytes = (long)st.st_size;
    return (long long)st.st_mtime;
}

/* text into a JSON string (quotes, backslashes, control characters) */
static size_t json_text(char *out, size_t size, const char *in)
{
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && w + 7 < size; ++p)
    {
        if (*p == '"' || *p == '\\')
            out[w++] = '\\', out[w++] = (char)*p;
        else if (*p < 0x20)
            w += (size_t)snprintf(out + w, size - w, "\\u%04x", *p);
        else
            out[w++] = (char)*p;
    }
    out[w] = '\0';
    return w;
}

static void read_note(const char *state, char *out, size_t size)
{
    char path[SwanStationPS5_PATH_MAX + 8];
    snprintf(path, sizeof(path), "%s.note", state);
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
}

/* GET /api/saves: every game with its card and the slots that hold a state */
static void send_saves(int fd)
{
    size_t cap = 64 * 1024 + (size_t)game_count * 2048, w = 0;
    char *out = malloc(cap);
    if (!out)
        return;
    SDL_LockMutex(lock);
    w += (size_t)snprintf(out + w, cap - w, "{\"running\":\"");
    w += json_text(out + w, cap - w, running_id);
    w += (size_t)snprintf(out + w, cap - w, "\",\"games\":[");
    for (int i = 0; i < game_count && w + 4096 < cap; ++i)
    {
        const RemoteGame *g = &games[i];
        char path[SwanStationPS5_PATH_MAX], text[400];
        card_file(g, path, sizeof(path));
        long long card = path[0] ? file_time(path, NULL) : 0;
        w += (size_t)snprintf(out + w, cap - w, "%s{\"id\":\"", i ? "," : "");
        w += json_text(out + w, cap - w, g->id);
        w += (size_t)snprintf(out + w, cap - w, "\",\"title\":\"");
        w += json_text(out + w, cap - w, g->title);
        w += (size_t)snprintf(out + w, cap - w, "\",\"serial\":\"%s\",\"card\":%lld,\"states\":[", g->serial,
                              card);
        bool first = true;
        for (int s = 0; s < 10; ++s)
        {
            state_file(g, s, path, sizeof(path));
            long bytes = 0;
            long long t = file_time(path, &bytes);
            if (!t)
                continue;
            char note[200];
            read_note(path, note, sizeof(note));
            json_text(text, sizeof(text), note);
            w += (size_t)snprintf(out + w, cap - w, "%s{\"slot\":%d,\"time\":%lld,\"bytes\":%ld,\"note\":\"%s\"}",
                                  first ? "" : ",", s, t, bytes, text);
            first = false;
        }
        w += (size_t)snprintf(out + w, cap - w, "]}");
    }
    SDL_UnlockMutex(lock);
    w += (size_t)snprintf(out + w, cap - w, "]}");
    respond(fd, "200 OK", "application/json", out, w);
    free(out);
}

static void send_file(int fd, const char *path, const char *download_name)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        respond(fd, "404 Not Found", "text/plain", "no such file", 12);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %ld\r\n"
                     "Content-Disposition: attachment; filename=\"%s\"\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     size, download_name);
    send_all(fd, head, (size_t)n);
    char buf[64 * 1024];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        send_all(fd, buf, got);
    fclose(f);
}

void *tdefl_write_image_to_png_file_in_memory(const void *image, int w, int h, int num_chans, size_t *len_out);
void mz_free(void *p);

/* GET /api/thumb: the slot's picture (PSXT, raw RGBA) as a PNG */
static void send_thumb(int fd, const char *state)
{
    char path[SwanStationPS5_PATH_MAX + 8];
    snprintf(path, sizeof(path), "%s.thumb", state);
    FILE *f = fopen(path, "rb");
    uint8_t head[8];
    if (!f || fread(head, 1, 8, f) != 8 || memcmp(head, "PSXT", 4) != 0)
    {
        if (f)
            fclose(f);
        respond(fd, "404 Not Found", "text/plain", "no picture", 10);
        return;
    }
    int w = head[4] | head[5] << 8, h = head[6] | head[7] << 8;
    uint8_t *rgba = w > 0 && h > 0 && w <= 1024 && h <= 1024 ? malloc((size_t)w * h * 4) : NULL;
    size_t len = 0;
    void *png = NULL;
    if (rgba && fread(rgba, 1, (size_t)w * h * 4, f) == (size_t)w * h * 4)
        png = tdefl_write_image_to_png_file_in_memory(rgba, w, h, 4, &len);
    fclose(f);
    free(rgba);
    if (png)
        respond(fd, "200 OK", "image/png", png, len);
    else
        respond(fd, "404 Not Found", "text/plain", "no picture", 10);
    mz_free(png);
}

/* The request's body: what came with the headers, then the rest. */
static char *read_body(int fd, const char *req, size_t req_len, size_t limit, size_t *len)
{
    const char *cl = strstr(req, "Content-Length:");
    if (!cl)
        cl = strstr(req, "content-length:");
    const char *end = strstr(req, "\r\n\r\n");
    if (!cl || !end)
        return NULL;
    long want = atol(cl + 15);
    if (want < 0 || (size_t)want > limit)
        return NULL;
    char *body = malloc((size_t)want + 1);
    if (!body)
        return NULL;
    size_t have = req_len - (size_t)(end + 4 - req);
    if (have > (size_t)want)
        have = (size_t)want;
    memcpy(body, end + 4, have);
    while (have < (size_t)want)
    {
        ssize_t n = recv(fd, body + have, (size_t)want - have, 0);
        if (n <= 0)
        {
            free(body);
            return NULL;
        }
        have += (size_t)n;
    }
    body[have] = '\0';
    *len = have;
    return body;
}

static bool write_atomic(const char *path, const char *data, size_t len)
{
    char temp[SwanStationPS5_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.upload", path);
    FILE *f = fopen(temp, "wb");
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f)
        ok = fclose(f) == 0 && ok;
    ok = ok && rename(temp, path) == 0;
    if (!ok)
        remove(temp);
    return ok;
}

/* /api/save?... : GET downloads, POST uploads; also thumb and note */
static void serve_saves(int fd, const char *method, const char *path, const char *req, size_t req_len)
{
    const char *q = strchr(path, '?');
    char id[64] = "", kind[8] = "", slot_s[8] = "";
    if (q)
    {
        query(q + 1, "game", id, sizeof(id));
        query(q + 1, "type", kind, sizeof(kind));
        query(q + 1, "slot", slot_s, sizeof(slot_s));
    }
    /* ids are written by SwanStationPS5 itself (serials or title letters), so no escapes to undo */
    SDL_LockMutex(lock);
    const RemoteGame *found = find_game(id);
    RemoteGame g;
    if (found)
        g = *found;
    bool running = found && !strcmp(running_id, id);
    SDL_UnlockMutex(lock);
    int slot = atoi(slot_s);
    if (!found || slot < 0 || slot > 9)
    {
        reply_json(fd, "404 Not Found", "{\"ok\":false,\"error\":\"unknown game\"}");
        return;
    }
    char file[SwanStationPS5_PATH_MAX], name[160];
    bool card = !strcmp(kind, "card");
    if (card)
    {
        card_file(&g, file, sizeof(file));
        if (!file[0] && g.serial[0])
        {
            char n[48];
            snprintf(n, sizeof(n), "%s_1.mcd", g.serial);
            path_join(file, sizeof(file), app.paths.saves, n);
        }
        snprintf(name, sizeof(name), "%s.mcd", g.serial[0] ? g.serial : g.id);
    }
    else
    {
        state_file(&g, slot, file, sizeof(file));
        snprintf(name, sizeof(name), "%s.state%d", g.id, slot);
    }
    if (!file[0])
    {
        reply_json(fd, "404 Not Found", "{\"ok\":false,\"error\":\"no card for this game\"}");
        return;
    }
    if (!strncmp(path, "/api/thumb", 10))
    {
        send_thumb(fd, file);
        return;
    }
    if (!strcmp(method, "GET"))
    {
        send_file(fd, file, name);
        return;
    }
    size_t len = 0;
    char *body = read_body(fd, req, req_len, !strncmp(path, "/api/note", 9) ? 1024 : 64u << 20, &len);
    if (!body)
    {
        reply_json(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"nothing received\"}");
        return;
    }
    bool ok;
    if (!strncmp(path, "/api/note", 9))
    {
        char note[SwanStationPS5_PATH_MAX + 8];
        snprintf(note, sizeof(note), "%s.note", file);
        len = strcspn(body, "\r\n");
        ok = len ? write_atomic(note, body, len < 120 ? len : 120) : (remove(note), true);
    }
    else if (running)
        ok = false;
    else
    {
        if (card) /* keep the card it replaces */
        {
            char backup[SwanStationPS5_PATH_MAX + 8];
            snprintf(backup, sizeof(backup), "%s.bak", file);
            FILE *old = fopen(file, "rb");
            if (old)
            {
                fclose(old);
                remove(backup);
                rename(file, backup);
            }
        }
        ok = (!card || len == 128 * 1024) && write_atomic(file, body, len);
    }
    free(body);
    SwanStationPS5_log("remote: %s %s for %s: %s", !strncmp(path, "/api/note", 9) ? "note" : card ? "card" : "state",
              !strcmp(method, "POST") ? "uploaded" : "?", id, ok ? "ok" : "refused");
    if (ok)
        reply_json(fd, "200 OK", "{\"ok\":true}");
    else if (running)
        reply_json(fd, "409 Conflict", "{\"ok\":false,\"error\":\"quit the game first\"}");
    else
        reply_json(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"not a memory card (128 KB) or not saved\"}");
}

/* RetroAchievements sign-in, done here on the server thread (it waits on the
 * network); the main thread then saves the token and signs in. */
static char login_user[64], login_token[128];
static bool login_ready, logout_ready;

static int hex_digit(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* a field of an application/x-www-form-urlencoded body, decoded */
static bool form_field(const char *body, const char *name, char *out, size_t size)
{
    size_t n = strlen(name);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
    {
        if (strncmp(p, name, n) != 0 || p[n] != '=')
            continue;
        size_t w = 0;
        for (const char *v = p + n + 1; *v && *v != '&' && w + 1 < size; ++v)
        {
            if (*v == '+')
                out[w++] = ' ';
            else if (*v == '%' && hex_digit(v[1]) >= 0 && hex_digit(v[2]) >= 0)
            {
                out[w++] = (char)(hex_digit(v[1]) * 16 + hex_digit(v[2]));
                v += 2;
            }
            else
                out[w++] = *v;
        }
        out[w] = '\0';
        return true;
    }
    return false;
}

static void serve_ra_login(int fd, const char *req, size_t req_len)
{
    size_t len = 0;
    char *body = read_body(fd, req, req_len, 2048, &len);
    char name[64] = "", password[256] = "";
    bool have = body && form_field(body, "user", name, sizeof(name)) &&
                form_field(body, "password", password, sizeof(password)) && name[0] && password[0];
    if (body)
    {
        memset(body, 0, len); /* the password was in there */
        free(body);
    }
    if (!have)
    {
        memset(password, 0, sizeof(password));
        const char *bad = "{\"ok\":false,\"error\":\"Enter your user name and password\"}";
        respond(fd, "400 Bad Request", "application/json", bad, strlen(bad));
        return;
    }
    char token[128] = "", who[64] = "", error[160] = "";
    bool ok = ra_password_login(name, password, token, sizeof(token), who, sizeof(who), error, sizeof(error));
    memset(password, 0, sizeof(password));
    if (ok)
    {
        SDL_LockMutex(lock);
        str_copy(login_user, sizeof(login_user), who);
        str_copy(login_token, sizeof(login_token), token);
        login_ready = true;
        SDL_UnlockMutex(lock);
    }
    memset(token, 0, sizeof(token));
    SwanStationPS5_log("remote: RetroAchievements sign-in from the phone: %s", ok ? "ok" : "failed");
    char answer[400];
    int n;
    if (ok)
        n = snprintf(answer, sizeof(answer), "{\"ok\":true,\"user\":\"%s\"}", who);
    else
    {
        for (char *c = error; *c; ++c)
            if (*c == '"' || *c == '\\')
                *c = '\'';
        n = snprintf(answer, sizeof(answer), "{\"ok\":false,\"error\":\"%s\"}", error);
    }
    respond(fd, ok ? "200 OK" : "401 Unauthorized", "application/json", answer, (size_t)n);
}

static void serve(int fd)
{
    char req[4096];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0)
        return;
    req[n] = '\0';
    char method[8] = "", path[512] = "";
    if (sscanf(req, "%7s %511s", method, path) != 2)
        return;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
    {
        size_t len;
        char *page = read_asset("remote.html", &len);
        if (page)
            respond(fd, "200 OK", "text/html; charset=utf-8", page, len);
        else
            respond(fd, "404 Not Found", "text/plain", "missing page", 12);
        free(page);
    }
    else if (!strcmp(path, "/api/settings"))
    {
        SDL_LockMutex(lock);
        char *copy = snapshot_len ? malloc(snapshot_len) : NULL;
        size_t len = snapshot_len;
        if (copy)
            memcpy(copy, snapshot, len);
        SDL_UnlockMutex(lock);
        if (copy)
            respond(fd, "200 OK", "application/json", copy, len);
        else
            respond(fd, "503 Service Unavailable", "application/json", "{}", 2);
        free(copy);
    }
    else if (!strcmp(path, "/api/saves"))
        send_saves(fd);
    else if (!strncmp(path, "/api/save?", 10) || !strncmp(path, "/api/thumb?", 11) ||
             (!strcmp(method, "POST") && !strncmp(path, "/api/note?", 10)))
        serve_saves(fd, method, path, req, (size_t)n);
    else if (!strcmp(method, "POST") && !strcmp(path, "/api/ra-login"))
        serve_ra_login(fd, req, (size_t)n);
    else if (!strcmp(method, "POST") && !strcmp(path, "/api/ra-logout"))
    {
        SDL_LockMutex(lock);
        logout_ready = true;
        SDL_UnlockMutex(lock);
        respond(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
    }
    else if (!strcmp(method, "POST") && !strncmp(path, "/api/set?", 9))
    {
        char key[16], value[16];
        if (query(path + 9, "key", key, sizeof(key)) && query(path + 9, "value", value, sizeof(value)))
        {
            SDL_LockMutex(lock);
            if (pending_count < PENDING)
            {
                str_copy(pending[pending_count].key, sizeof(pending[0].key), key);
                pending[pending_count++].value = atoi(value);
            }
            SDL_UnlockMutex(lock);
            respond(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
        }
        else
            respond(fd, "400 Bad Request", "application/json", "{\"ok\":false}", 12);
    }
    else
        respond(fd, "404 Not Found", "text/plain", "not found", 9);
}

static int server(void *unused)
{
    (void)unused;
    while (!SDL_AtomicGet(&quit))
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listen_fd, &set);
        struct timeval tv = {0, 300000};
        if (select(listen_fd + 1, &set, NULL, NULL, &tv) <= 0)
            continue;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        struct timeval to = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        serve(fd);
        close(fd);
    }
    return 0;
}

/* The console's address on the local network: the source address a UDP
 * socket would use towards the internet (nothing is sent). */
static void find_address(void)
{
    address[0] = '\0';
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808u);
    struct sockaddr_in me = {0};
    socklen_t len = sizeof(me);
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 &&
        getsockname(s, (struct sockaddr *)&me, &len) == 0)
    {
        uint32_t a = ntohl(me.sin_addr.s_addr);
        snprintf(address, sizeof(address), "http://%u.%u.%u.%u:%d/", a >> 24, (a >> 16) & 255,
                 (a >> 8) & 255, a & 255, REMOTE_PORT);
    }
    close(s);
}

void remote_update(bool enabled)
{
    if (enabled == (thread != NULL))
        return;
    if (!enabled)
    {
        SDL_AtomicSet(&quit, 1);
        SDL_WaitThread(thread, NULL);
        thread = NULL;
        close(listen_fd);
        listen_fd = -1;
        address[0] = '\0';
        return;
    }
    if (!lock)
        lock = SDL_CreateMutex();
    if (!snapshot)
        snapshot = malloc(SNAPSHOT_SIZE);
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0 || !lock || !snapshot)
    {
        SwanStationPS5_log("remote: no socket");
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REMOTE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 4) != 0)
    {
        SwanStationPS5_log("remote: port %d unavailable", REMOTE_PORT);
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    SDL_AtomicSet(&quit, 0);
    find_address();
    thread = SDL_CreateThread(server, "remote", NULL);
    SwanStationPS5_log("remote: settings page at %s", address[0] ? address : "(no address)");
}

void remote_frame(void)
{
    if (!thread)
        return;
    static uint64_t refreshed;
    uint64_t now = plat_ticks_us();
    SDL_LockMutex(lock);
    int count = pending_count;
    char keys[PENDING][16];
    int values[PENDING];
    for (int i = 0; i < count; ++i)
    {
        str_copy(keys[i], sizeof(keys[i]), pending[i].key);
        values[i] = pending[i].value;
    }
    pending_count = 0;
    SDL_UnlockMutex(lock);
    /* a sign-in or sign-out from the phone */
    char user_in[64] = "", token_in[128] = "";
    bool signing_in = false, signing_out = false;
    SDL_LockMutex(lock);
    if (login_ready)
    {
        str_copy(user_in, sizeof(user_in), login_user);
        str_copy(token_in, sizeof(token_in), login_token);
        memset(login_token, 0, sizeof(login_token));
        login_ready = false;
        signing_in = true;
    }
    signing_out = logout_ready;
    logout_ready = false;
    SDL_UnlockMutex(lock);
    if (signing_in)
    {
        ra_use_token(user_in, token_in);
        memset(token_in, 0, sizeof(token_in));
        char msg[160];
        snprintf(msg, sizeof(msg), tr("Signed in to RetroAchievements as %s"), user_in);
        app_toast(msg);
        refreshed = 0; /* show it on the phone */
    }
    if (signing_out)
    {
        ra_sign_out();
        app_toast("Signed out of RetroAchievements");
        refreshed = 0;
    }
    for (int i = 0; i < count; ++i)
    {
        const char *name = settings_set_by_key(keys[i], values[i]);
        if (name)
        {
            char msg[160];
            snprintf(msg, sizeof(msg), tr("Changed from your phone: %s"), tr(name));
            app_toast(msg);
            app_save_settings();
        }
    }
    static uint64_t games_at;
    if (!games_at || now - games_at > 5000000)
    {
        games_at = now;
        SDL_LockMutex(lock);
        if (!games)
            games = calloc(REMOTE_GAMES, sizeof(RemoteGame));
        game_count = 0;
        for (int i = 0; games && i < app.library.count && i < REMOTE_GAMES; ++i)
        {
            const Game *g = &app.library.games[i];
            str_copy(games[game_count].id, sizeof(games[0].id), g->id);
            str_copy(games[game_count].title, sizeof(games[0].title), g->title);
            str_copy(games[game_count].serial, sizeof(games[0].serial), g->serial);
            ++game_count;
        }
        str_copy(running_id, sizeof(running_id), app.game ? app.game->id : "");
        SDL_UnlockMutex(lock);
    }
    else if (app.game ? strcmp(running_id, app.game->id) : running_id[0])
    {
        SDL_LockMutex(lock);
        str_copy(running_id, sizeof(running_id), app.game ? app.game->id : "");
        SDL_UnlockMutex(lock);
    }
    if (count || now - refreshed > 500000)
    {
        static char fresh[SNAPSHOT_SIZE];
        int len = settings_json(fresh, sizeof(fresh));
        SDL_LockMutex(lock);
        memcpy(snapshot, fresh, (size_t)len);
        snapshot_len = (size_t)len;
        SDL_UnlockMutex(lock);
        refreshed = now;
    }
}

const char *remote_address(void)
{
    return address;
}

#endif
