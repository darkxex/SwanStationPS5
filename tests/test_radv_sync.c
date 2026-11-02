/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

int ss_radv_cnd_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);

static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool signaled;

int64_t os_time_get_nano(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

/* Stand in for Mesa's C11 adapter, retaining pthread's result for assertions. */
int cnd_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline)
{
    assert(deadline->tv_nsec >= 0 && deadline->tv_nsec < 1000000000L);
    return pthread_cond_timedwait(c, m, deadline);
}

static struct timespec after(int64_t ns)
{
    const int64_t at = os_time_get_nano() + ns;
    return (struct timespec){(time_t)(at / INT64_C(1000000000)), (long)(at % INT64_C(1000000000))};
}

static void *signal_waiter(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&mutex) == 0);
    signaled = true;
    assert(pthread_cond_signal(&cond) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);
    return NULL;
}

int main(void)
{
    assert(pthread_mutex_lock(&mutex) == 0);
    struct timespec deadline = after(INT64_C(30000000));
    const int64_t began = os_time_get_nano();
    int result;
    do
        result = ss_radv_cnd_timedwait(&cond, &mutex, &deadline);
    while (result == 0);
    assert(result == ETIMEDOUT);
    assert(os_time_get_nano() - began >= INT64_C(25000000));

    deadline = after(-INT64_C(1000000));
    assert(ss_radv_cnd_timedwait(&cond, &mutex, &deadline) == ETIMEDOUT);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, signal_waiter, NULL) == 0);
    deadline = after(INT64_C(1100000000));
    while (!signaled)
        assert(ss_radv_cnd_timedwait(&cond, &mutex, &deadline) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(pthread_cond_destroy(&cond) == 0);
    assert(pthread_mutex_destroy(&mutex) == 0);
    return 0;
}
