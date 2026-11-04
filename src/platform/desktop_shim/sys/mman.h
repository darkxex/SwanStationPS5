/*
 * SwanStationPS5 desktop build - the anonymous-memory corner of <sys/mman.h> on Windows (discimport.c's RAM buffers).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_SHIM_MMAN_H
#define SwanStationPS5_SHIM_MMAN_H
#include <stddef.h>
#include <windows.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_ANON 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)
static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    (void)addr, (void)prot, (void)flags, (void)fd, (void)off;
    void *p = VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p ? p : MAP_FAILED;
}
static inline int munmap(void *addr, size_t len)
{
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}
#endif
