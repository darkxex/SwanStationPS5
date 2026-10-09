/*
 * SwanStationPS5 - asking the HEN to let SwanStationPS5 list /data.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A sandboxed title can open and write files under /data but can't list
 * folders. Several services lift that, depending on what the console runs:
 *  - LegacyJB (Phoenixx) and OnionHEN watch each sandbox for a request file,
 *    {"PID":<pid>} in /download0/etahen_jailbreak, for any app;
 *  - PS5SX2 Helper answers the same file, but only for the title IDs in
 *    /data/whitelist.txt (Settings > System can add SwanStationPS5 there);
 *  - etaHEN answers a HijackerCommand on TCP 127.0.0.1:9028 when its "Legacy
 *    CMD server" setting is on (LegacyJB listens there too).
 * The file goes first; the TCP command when nothing answered it. Porpoise was killed right after its grant; it asks from a process that
 * already runs threads, so SwanStationPS5 asks first thing in main(), single-threaded,
 * after giving itself its own credential.
 *
 * A marker file guards against a crash loop: it is written before asking and
 * removed afterwards. If SwanStationPS5 finds it at start-up, the previous request
 * killed the app, so this route is skipped and SwanStationPS5 runs sandboxed.
 */
#if defined(__PROSPERO__)
#include "ps5_unlock.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int sceKernelUsleep(unsigned microseconds);

static const char *const REQUESTS[] = {
    "/download0/etahen_jailbreak",   /* etaHEN, OnionHEN, old Lapy */
    "/download0/onionhen_jailbreak", /* OnionHEN also takes this name */
};
#define REQUEST_COUNT (int)(sizeof(REQUESTS) / sizeof(REQUESTS[0]))
#define CRASH_MARKER "/download0/SwanStationPS5_unlock_attempt"

bool ps5_data_listable(void)
{
    DIR *d = opendir("/data");
    if (!d)
        return false;
    closedir(d);
    return true;
}

static bool publish(const char *path, int pid)
{
    char staged[64];
    snprintf(staged, sizeof(staged), "%s.tmp", path);
    unlink(staged);
    unlink(path);
    int fd = open(staged, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return false;
    fchmod(fd, 0666);
    char body[32];
    int n = snprintf(body, sizeof(body), "{\"PID\":%d}\n", pid);
    bool ok = write(fd, body, (size_t)n) == n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(staged, path) != 0)
    {
        unlink(staged);
        return false;
    }
    return true;
}

/* etaHEN's legacy command server (README: "Jailbreaking an app ... non-
 * whitelist method"). Same layout as its HijackerCommand. */
struct HijackerCommand
{
    int magic; /* 0xDEADBEEF */
    int cmd;   /* JAILBREAK_CMD = 5 */
    int pid;
    int ret;   /* -1337 until answered */
    char msg1[0x500];
    char msg2[0x500];
};

static bool request_over_tcp(int pid)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return false;
    struct timeval tv = {2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons(9028);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0)
    {
        static struct HijackerCommand c;
        memset(&c, 0, sizeof(c));
        c.magic = (int)0xDEADBEEF;
        c.cmd = 5;
        c.pid = pid;
        c.ret = -1337;
        if (send(s, &c, sizeof(c), 0) == (ssize_t)sizeof(c))
        {
            size_t got = 0;
            while (got < sizeof(c))
            {
                ssize_t n = recv(s, (char *)&c + got, sizeof(c) - got, 0);
                if (n <= 0)
                    break;
                got += (size_t)n;
            }
            ok = got >= 16 && (c.ret == 0 || c.ret == -1337);
        }
    }
    close(s);
    return ok;
}

static bool wait_listable(int polls)
{
    for (int poll = 0; poll < polls; ++poll)
    {
        sceKernelUsleep(16667);
        if (ps5_data_listable())
            return true;
    }
    return false;
}

UnlockResult ps5_unlock_etahen(void)
{
    if (ps5_data_listable())
        return UNLOCK_ALREADY;
    if (access(CRASH_MARKER, F_OK) == 0)
    {
        /* The previous launch died while asking: skip this one launch (no crash
         * loop), then ask again on the next. */
        unlink(CRASH_MARKER);
        return UNLOCK_SKIPPED_AFTER_CRASH;
    }
    int marker = open(CRASH_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (marker >= 0)
    {
        fsync(marker);
        close(marker);
    }

    seteuid(geteuid()); /* own credential before the HEN changes it */
    UnlockResult result = UNLOCK_NO_ANSWER;
    const int pid = (int)getpid();
    for (int round = 0; round < 3 && result != UNLOCK_OK; ++round)
    {
        int published = 0;
        for (int i = 0; i < REQUEST_COUNT; ++i)
            published += publish(REQUESTS[i], pid);
        if (published == 0)
        {
            result = UNLOCK_CANT_REQUEST;
            break;
        }
        /* ~1.5 s for a service to act and /data to open up */
        if (wait_listable(90))
            result = UNLOCK_OK;
        for (int i = 0; i < REQUEST_COUNT; ++i)
            unlink(REQUESTS[i]);
        if (result != UNLOCK_OK && round == 0 && request_over_tcp(pid) && wait_listable(60))
            result = UNLOCK_OK; /* etaHEN's command server */
    }
    unlink(CRASH_MARKER); /* we survived the request either way */
    return result;
}

void ps5_unlock_reset_crash_marker(void)
{
    unlink(CRASH_MARKER);
}

const char *ps5_unlock_describe(UnlockResult r)
{
    switch (r)
    {
    case UNLOCK_OK: return "granted";
    case UNLOCK_ALREADY: return "already unlocked";
    case UNLOCK_NO_ANSWER: return "no unlock service answered (LegacyJB, etaHEN, PS5SX2 Helper)";
    case UNLOCK_CANT_REQUEST: return "couldn't write the request";
    case UNLOCK_SKIPPED_AFTER_CRASH: return "skipped once after the last request closed SwanStationPS5 (start again to retry)";
    }
    return "?";
}
#endif
