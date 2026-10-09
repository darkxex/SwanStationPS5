/*
 * SwanStationPS5 - writes a crash report into SwanStationPS5.log on the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Shell only says "something went wrong". On a fatal signal this logs
 * the signal, the faulting address, the instruction pointer and a short
 * frame-pointer walk, each also as an offset from main(), so the CI build's
 * symbol map (SwanStationPS5-symbols.txt) turns them into function names.
 */
#if defined(__PROSPERO__)
#include "ps5_crash.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

extern int main(void);

static int log_fd = -1;
static const char *last_step = "start";

void ps5_crash_step(const char *step)
{
    last_step = step;
}

static void put(const char *s)
{
    if (log_fd >= 0)
        (void)write(log_fd, s, strlen(s));
}

static void report_address(const char *label, uintptr_t addr)
{
    char line[160];
    intptr_t offset = (intptr_t)addr - (intptr_t)(uintptr_t)&main;
    snprintf(line, sizeof(line), "  %s 0x%llx (main%+lld)\n", label, (unsigned long long)addr,
             (long long)offset);
    put(line);
}

static void on_crash(int sig, siginfo_t *info, void *context)
{
    char line[200];
    snprintf(line, sizeof(line), "CRASH: signal %d at address %p during '%s'; main at %p\n", sig,
             info ? info->si_addr : NULL, last_step, (void *)(uintptr_t)&main);
    put(line);
    ucontext_t *uc = (ucontext_t *)context;
    if (uc)
    {
        uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
        uintptr_t rbp = (uintptr_t)uc->uc_mcontext.mc_rbp;
        uintptr_t rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
        report_address("rip", rip);
        /* return addresses near the top of the stack (works without frame pointers) */
        uintptr_t *stack = (uintptr_t *)rsp;
        uintptr_t lo = (uintptr_t)&main - 0x4000000, hi = (uintptr_t)&main + 0x4000000;
        int found = 0;
        for (int i = 0; i < 512 && found < 12; ++i)
        {
            uintptr_t v = stack[i];
            if (v > lo && v < hi)
            {
                report_address("stack", v);
                ++found;
            }
        }
        (void)rbp;
    }
    put("END CRASH\n");
    if (log_fd >= 0)
        fsync(log_fd);
    signal(sig, SIG_DFL);
    raise(sig);
}

void ps5_crash_install(const char *log_path)
{
    log_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        sigaction(signals[i], &sa, NULL);
}
#endif
