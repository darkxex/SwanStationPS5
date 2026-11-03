/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(__PROSPERO__) || defined(SwanStationPS5_RADV_SYNC_TEST)
#if defined(SwanStationPS5_VULKAN)
#include <pthread.h>
#include <stdint.h>
#include <time.h>

extern int64_t os_time_get_nano(void);
extern int cnd_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);

/* Only the pinned driver's WSI and GPU waits pass monotonic deadlines to C11's realtime wait. */
int ss_radv_cnd_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline)
{
    struct timespec realtime;
    const int64_t remaining = (int64_t)deadline->tv_sec * INT64_C(1000000000) + deadline->tv_nsec - os_time_get_nano();
    if (clock_gettime(CLOCK_REALTIME, &realtime) != 0)
        return cnd_timedwait(cond, mutex, deadline);
    if (remaining > 0)
    {
        realtime.tv_sec += remaining / INT64_C(1000000000);
        realtime.tv_nsec += remaining % INT64_C(1000000000);
        if (realtime.tv_nsec >= 1000000000L)
        {
            ++realtime.tv_sec;
            realtime.tv_nsec -= 1000000000L;
        }
    }
    return cnd_timedwait(cond, mutex, &realtime);
}
#endif
#endif
