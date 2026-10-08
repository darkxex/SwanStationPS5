/*
 * PSXS5 - RetroAchievements (retroachievements.org) through rcheevos.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * - Sign-in uses the token from retroachievements.ini; the password is never
 *   stored on the console (tools/psxs5_sync.py ra-login exchanges it once).
 * - Server calls run on one worker thread (libcurl, blocking).
 * - PS1 RAM is read through the core's libretro memory map (rc_libretro).
 * - The game is identified by rc_hash's PlayStation hash, reading sectors
 *   through the core so every image format works (bin/cue, CHD, PBP).
 * - Events are queued and shown by the main thread.
 */
#include "achievements.h"

#include "../core/host.h"
#include "../i18n.h"
#include "../net.h"

#include "libretro.h"
#include "rc_client.h"
#include "rc_hash.h"
#include "../../third_party/rcheevos/src/rc_libretro.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static rc_client_t *client;
static Paths paths;
static char user[64], token[128];
static bool hardcore;
static bool signed_in;
static rc_libretro_memory_regions_t regions;
static bool regions_ready;
static char agent[256];

/* ---------------------------------------------------------------- messages */

#define MESSAGES 16
typedef struct
{
    char title[96];
    char detail[192];
} Message;

static Message messages[MESSAGES];
static int msg_head, msg_count;
static SDL_mutex *msg_lock;
/* the progress indicator rcheevos raises when a counted achievement moves */
static struct
{
    char title[96], progress[24];
    float percent;
    Uint64 until;
} tracker;

bool ra_tracker(char *title, size_t title_size, char *progress, size_t progress_size, float *percent)
{
    if (!msg_lock)
        return false;
    SDL_LockMutex(msg_lock);
    bool on = tracker.until > SDL_GetTicks64() && tracker.progress[0];
    if (on)
    {
        str_copy(title, title_size, tracker.title);
        str_copy(progress, progress_size, tracker.progress);
        *percent = tracker.percent;
    }
    SDL_UnlockMutex(msg_lock);
    return on;
}

static void post(const char *title, const char *detail)
{
    psxs5_log("ra: %s - %s", title, detail);
    if (!msg_lock)
        return;
    SDL_LockMutex(msg_lock);
    if (msg_count < MESSAGES)
    {
        Message *m = &messages[(msg_head + msg_count++) % MESSAGES];
        str_copy(m->title, sizeof(m->title), title);
        str_copy(m->detail, sizeof(m->detail), detail);
    }
    SDL_UnlockMutex(msg_lock);
}

bool ra_next_message(char *title, size_t title_size, char *detail, size_t detail_size)
{
    if (!msg_lock)
        return false;
    SDL_LockMutex(msg_lock);
    bool any = msg_count > 0;
    if (any)
    {
        str_copy(title, title_size, messages[msg_head].title);
        str_copy(detail, detail_size, messages[msg_head].detail);
        msg_head = (msg_head + 1) % MESSAGES;
        --msg_count;
    }
    SDL_UnlockMutex(msg_lock);
    return any;
}

/* ---------------------------------------------------------------- server calls */

typedef struct Request
{
    char *url, *post_data, *content_type;
    rc_client_server_callback_t callback;
    void *callback_data;
    struct Request *next;
} Request;

static Request *queue_head, *queue_tail;
static SDL_mutex *queue_lock;
static SDL_cond *queue_cond;
static SDL_Thread *worker;
static bool quitting;

static char *dup_or_null(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static int worker_main(void *unused)
{
    (void)unused;
    for (;;)
    {
        SDL_LockMutex(queue_lock);
        while (!queue_head && !quitting)
            SDL_CondWait(queue_cond, queue_lock);
        if (quitting)
        {
            SDL_UnlockMutex(queue_lock);
            return 0;
        }
        Request *r = queue_head;
        queue_head = r->next;
        if (!queue_head)
            queue_tail = NULL;
        SDL_UnlockMutex(queue_lock);

        char *body = NULL;
        size_t length = 0;
        int status = net_request(r->url, r->post_data, r->content_type, agent, &body, &length);
        rc_api_server_response_t response;
        response.body = body ? body : "";
        response.body_length = length;
        /* rc_client treats this code as "retry later" rather than a hard failure */
        response.http_status_code = status > 0 ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        r->callback(&response, r->callback_data);
        free(body);
        free(r->url);
        free(r->post_data);
        free(r->content_type);
        free(r);
    }
}

static void RC_CCONV server_call(const rc_api_request_t *request,
                                 rc_client_server_callback_t callback, void *callback_data,
                                 rc_client_t *c)
{
    (void)c;
    Request *r = calloc(1, sizeof(*r));
    if (!r)
        return;
    r->url = dup_or_null(request->url);
    r->post_data = dup_or_null(request->post_data);
    r->content_type = dup_or_null(request->content_type);
    r->callback = callback;
    r->callback_data = callback_data;
    SDL_LockMutex(queue_lock);
    if (queue_tail)
        queue_tail->next = r;
    else
        queue_head = r;
    queue_tail = r;
    SDL_CondSignal(queue_cond);
    SDL_UnlockMutex(queue_lock);
}

/* ---------------------------------------------------------------- memory */

static void RC_CCONV core_memory_info(uint32_t id, rc_libretro_core_memory_info_t *info)
{
    info->data = host_memory_data(id);
    info->size = host_memory_size(id);
}

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes,
                                     rc_client_t *c)
{
    (void)c;
    if (!regions_ready)
        return 0;
    return rc_libretro_memory_read(&regions, address, buffer, num_bytes);
}

/* ---------------------------------------------------------------- disc hashing */

/* rcheevos reads the image file itself (bin/cue, iso). */

/* ---------------------------------------------------------------- events */

/* Each unlock, for the profile's "Recently unlocked": <root>/achievements-log.txt,
 * time<TAB>points<TAB>game<TAB>achievement<TAB>description. */
static void log_unlock(const rc_client_achievement_t *a)
{
    char path[PSXS5_PATH_MAX];
    path_join(path, sizeof(path), paths.user, "achievements-log.txt");
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    const rc_client_game_t *g = client ? rc_client_get_game_info(client) : NULL;
    char game[96], title[96], description[192];
    const char *src[3] = {g && g->title ? g->title : "", a->title ? a->title : "", a->description ? a->description : ""};
    char *dst[3] = {game, title, description};
    size_t cap[3] = {sizeof(game), sizeof(title), sizeof(description)};
    for (int k = 0; k < 3; ++k)
    {
        str_copy(dst[k], cap[k], src[k]);
        for (char *p = dst[k]; *p; ++p)
            if (*p == '\t' || *p == '\n' || *p == '\r')
                *p = ' ';
    }
    fprintf(f, "%lld\t%u\t%s\t%s\t%s\n", (long long)time(NULL), a->points, game, title, description);
    fclose(f);
}

int ra_recent(RaRecent *out, int max)
{
    char path[PSXS5_PATH_MAX];
    path_join(path, sizeof(path), paths.user, "achievements-log.txt");
    FILE *f = fopen(path, "r");
    if (!f || max <= 0)
    {
        if (f)
            fclose(f);
        return 0;
    }
    /* keep the last `max` lines, newest first */
    int n = 0, at = 0;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *field[5] = {0};
        char *p = line;
        line[strcspn(line, "\r\n")] = '\0';
        for (int i = 0; i < 5 && p; ++i)
        {
            field[i] = p;
            p = strchr(p, '\t');
            if (p)
                *p++ = '\0';
        }
        if (!field[3])
            continue;
        RaRecent *r = &out[at];
        r->when = atoll(field[0]);
        r->points = (unsigned)strtoul(field[1], NULL, 10);
        str_copy(r->game, sizeof(r->game), field[2]);
        str_copy(r->title, sizeof(r->title), field[3]);
        str_copy(r->description, sizeof(r->description), field[4] ? field[4] : "");
        at = (at + 1) % max;
        if (n < max)
            ++n;
    }
    fclose(f);
    /* the ring holds the newest at at-1: turn it into newest first */
    RaRecent tmp[64];
    int count = n < 64 ? n : 64;
    for (int i = 0; i < count; ++i)
        tmp[i] = out[((at - 1 - i) % max + max) % max];
    memcpy(out, tmp, sizeof(RaRecent) * (size_t)count);
    return count;
}

unsigned ra_user_softcore_score(void)
{
    if (!client || !signed_in)
        return 0;
    const rc_client_user_t *u = rc_client_get_user_info(client);
    return u ? u->score_softcore : 0;
}

unsigned ra_user_hardcore_score(void)
{
    if (!client || !signed_in)
        return 0;
    const rc_client_user_t *u = rc_client_get_user_info(client);
    return u ? u->score : 0;
}

static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *c)
{
    char detail[192];
    switch (e->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        snprintf(detail, sizeof(detail), tr("%s  (%u points)"), e->achievement->description,
                 e->achievement->points);
        post(e->achievement->title, detail);
        log_unlock(e->achievement);
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
        if (msg_lock && e->achievement)
        {
            SDL_LockMutex(msg_lock);
            str_copy(tracker.title, sizeof(tracker.title), e->achievement->title ? e->achievement->title : "");
            str_copy(tracker.progress, sizeof(tracker.progress), e->achievement->measured_progress);
            tracker.percent = e->achievement->measured_percent;
            tracker.until = SDL_GetTicks64() + 4000;
            SDL_UnlockMutex(msg_lock);
        }
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
        break; /* shown for 4 s anyway, long enough to read */
    case RC_CLIENT_EVENT_GAME_COMPLETED:
    {
        const rc_client_game_t *g = rc_client_get_game_info(c);
        post(tr(hardcore ? "Mastered!" : "Completed!"), g ? g->title : "");
        break;
    }
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
        post(tr("Leaderboard attempt started"), e->leaderboard->title);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
        post(tr("Leaderboard attempt failed"), e->leaderboard->title);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        snprintf(detail, sizeof(detail), "%s: %s", e->leaderboard->title,
                 e->leaderboard->tracker_value);
        post(tr("Leaderboard score submitted"), detail);
        break;
    case RC_CLIENT_EVENT_RESET:
        host_reset(); /* switching to hardcore restarts the game */
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        post(tr("RetroAchievements error"), e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        post(tr("RetroAchievements offline"), tr("Unlocks will be sent when the connection is back."));
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        post(tr("RetroAchievements online"), tr("Pending unlocks were sent."));
        break;
    default:
        break;
    }
}

static void RC_CCONV hash_error(const char *message)
{
    psxs5_log("ra: disc hash: %s", message);
}

static void RC_CCONV log_message(const char *message, const rc_client_t *c)
{
    (void)c;
    psxs5_log("rcheevos: %s", message);
}

/* ---------------------------------------------------------------- settings file */

static void ini_path(char *out, size_t size)
{
    path_join(out, size, paths.user, "retroachievements.ini"); /* the profile's sign-in */
}

static void load_ini(void)
{
    char path[PSXS5_PATH_MAX];
    ini_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (!strcmp(line, "user"))
            str_copy(user, sizeof(user), eq + 1);
        else if (!strcmp(line, "token"))
            str_copy(token, sizeof(token), eq + 1);
        else if (!strcmp(line, "hardcore"))
            hardcore = atoi(eq + 1) != 0;
    }
    fclose(f);
}

static void save_ini(void)
{
    char path[PSXS5_PATH_MAX];
    ini_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# RetroAchievements sign-in (token only, never the password)\nuser=%s\ntoken=%s\nhardcore=%d\n",
            user, token, hardcore ? 1 : 0);
    fclose(f);
}

/* ---------------------------------------------------------------- API */

static void RC_CCONV on_login(int result, const char *error, rc_client_t *c, void *userdata)
{
    (void)c;
    (void)userdata;
    signed_in = result == RC_OK;
    if (signed_in)
        post("RetroAchievements", tr("Signed in."));
    else
        post(tr("RetroAchievements sign-in failed"), error ? error : tr("check the token"));
}

void ra_init(const Paths *p)
{
    paths = *p;
    /* again after a profile change: the other person's sign-in */
    user[0] = token[0] = '\0';
    hardcore = false;
    signed_in = false;
    quitting = false;
    worker = NULL;
    load_ini();
    if (!user[0] || !token[0])
    {
        psxs5_log("ra: not set up (run tools/psxs5_sync.py ra-login)");
        return;
    }
    if (!msg_lock)
        msg_lock = SDL_CreateMutex();
    if (!queue_lock)
        queue_lock = SDL_CreateMutex();
    if (!queue_cond)
        queue_cond = SDL_CreateCond();
    client = rc_client_create(read_memory, server_call);
    if (!client || !msg_lock || !queue_lock || !queue_cond)
        return;
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(client, on_event);
    rc_client_set_hardcore_enabled(client, hardcore ? 1 : 0);
    char clause[128] = "";
    rc_client_get_user_agent_clause(client, clause, sizeof(clause));
    snprintf(agent, sizeof(agent), PSXS5_NAME "/" PSXS5_VERSION " (PS5) %s", clause);

    rc_hash_init_error_message_callback(hash_error);

    worker = SDL_CreateThread(worker_main, "retroachievements", NULL);
    rc_client_begin_login_with_token(client, user, token, on_login, NULL);
}

void ra_shutdown(void)
{
    if (!client)
        return;
    ra_game_unloaded();
    SDL_LockMutex(queue_lock);
    quitting = true;
    SDL_CondSignal(queue_cond);
    SDL_UnlockMutex(queue_lock);
    if (worker)
        SDL_WaitThread(worker, NULL);
    rc_client_destroy(client);
    client = NULL;
}

static void RC_CCONV on_game_loaded(int result, const char *error, rc_client_t *c, void *userdata)
{
    (void)userdata;
    if (result != RC_OK)
    {
        if (result == RC_NO_GAME_LOADED) /* "Unknown game": this dump's hash isn't in any set */
            post("RetroAchievements", tr("This version of the game isn't supported. A Redump dump of your disc usually is."));
        else
            psxs5_log("ra: no achievements loaded: %s", error ? error : "?");
        return;
    }
    const rc_client_game_t *g = rc_client_get_game_info(c);
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(c, &s);
    char detail[192];
    if (s.num_core_achievements)
        snprintf(detail, sizeof(detail), tr("%s: %u of %u achievements unlocked%s"), g ? g->title : "",
                 s.num_unlocked_achievements, s.num_core_achievements,
                 hardcore ? tr(" (hardcore)") : "");
    else
        snprintf(detail, sizeof(detail), tr("%s has no achievements yet"), g ? g->title : tr("This game"));
    post("RetroAchievements", detail);
}

/* The image rcheevos reads when the core can't hand it sectors: the first
 * disc of a .m3u playlist (one path per line, relative to the playlist). */
static void first_disc(const char *game_path, char *out, size_t size)
{
    str_copy(out, size, game_path);
    if (str_icmp(path_ext(game_path), "m3u") != 0)
        return;
    FILE *f = fopen(game_path, "r");
    char line[PSXS5_PATH_MAX] = "";
    while (f && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && line[0] != '#')
            break;
        line[0] = '\0';
    }
    if (f)
        fclose(f);
    if (!line[0])
        return;
    if (line[0] == '/')
        str_copy(out, size, line);
    else
    {
        char dir[PSXS5_PATH_MAX];
        str_copy(dir, sizeof(dir), game_path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = '\0';
        path_join(out, size, dir, line);
    }
}

void ra_game_loaded(const char *game_path)
{
    if (!client)
        return;
    regions_ready = rc_libretro_memory_init(&regions, host_memory_map(), core_memory_info,
                                            RC_CONSOLE_PLAYSTATION) != 0;
    char hash[33] = "", disc[PSXS5_PATH_MAX];
    first_disc(game_path, disc, sizeof(disc));
    bool ok = rc_hash_generate_from_file(hash, RC_CONSOLE_PLAYSTATION, disc);
    if (!ok)
    {
        psxs5_log("ra: could not hash this disc");
        return;
    }
    psxs5_log("ra: game hash %s", hash);
    rc_client_begin_load_game(client, hash, on_game_loaded, NULL);
}

void ra_game_unloaded(void)
{
    if (!client)
        return;
    rc_client_unload_game(client);
    if (regions_ready)
        rc_libretro_memory_destroy(&regions);
    regions_ready = false;
}

void ra_frame(void)
{
    if (client && regions_ready)
        rc_client_do_frame(client);
}

void ra_idle(void)
{
    if (client)
        rc_client_idle(client);
}

void ra_reset(void)
{
    if (client)
        rc_client_reset(client);
}

bool ra_signed_in(void)
{
    return signed_in;
}

const char *ra_user(void)
{
    return user;
}

bool ra_hardcore(void)
{
    return client && hardcore;
}

void ra_use_token(const char *name, const char *new_token)
{
    str_copy(user, sizeof(user), name);
    str_copy(token, sizeof(token), new_token);
    save_ini(); /* the profile's retroachievements.ini */
    ra_shutdown();
    ra_init(&paths);
}

void ra_sign_out(void)
{
    user[0] = token[0] = '\0';
    save_ini();
    ra_shutdown();
    signed_in = false;
}

void ra_set_hardcore(bool on)
{
    hardcore = on;
    if (client)
        rc_client_set_hardcore_enabled(client, on ? 1 : 0);
    if (user[0])
        save_ini();
}

void ra_game_summary(char *out, size_t size)
{
    out[0] = '\0';
    if (!client || !rc_client_get_game_info(client))
        return;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(client, &s);
    if (s.num_core_achievements)
        snprintf(out, size, "%u of %u achievements, %u of %u points", s.num_unlocked_achievements,
                 s.num_core_achievements, s.points_unlocked, s.points_core);
}

unsigned ra_user_score(void)
{
    if (!client || !signed_in)
        return 0;
    const rc_client_user_t *u = rc_client_get_user_info(client);
    return u ? (hardcore ? u->score : u->score_softcore) : 0;
}

bool ra_game_progress(int *unlocked, int *total)
{
    if (!client || !rc_client_get_game_info(client))
        return false;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(client, &s);
    if (!s.num_core_achievements)
        return false;
    *unlocked = (int)s.num_unlocked_achievements;
    *total = (int)s.num_core_achievements;
    return true;
}

int ra_list(RaAchievement *out, int max)
{
    if (!client || !rc_client_get_game_info(client))
        return 0;
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(
        client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list)
        return 0;
    int n = 0;
    for (uint32_t b = 0; b < list->num_buckets; ++b)
        for (uint32_t i = 0; i < list->buckets[b].num_achievements && n < max; ++i)
        {
            const rc_client_achievement_t *a = list->buckets[b].achievements[i];
            RaAchievement *r = &out[n++];
            str_copy(r->title, sizeof(r->title), a->title ? a->title : "");
            str_copy(r->description, sizeof(r->description), a->description ? a->description : "");
            str_copy(r->progress, sizeof(r->progress), a->measured_progress);
            r->percent = a->measured_percent;
            r->points = a->points;
            r->unlocked = a->unlocked != 0;
            r->id = a->id;
            r->badge_url[0] = '\0';
            rc_client_achievement_get_image_url(a, a->unlocked ? RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED
                                                               : RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE,
                                                r->badge_url, sizeof(r->badge_url));
        }
    rc_client_destroy_achievement_list(list);
    return n;
}
