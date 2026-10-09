/*
 * SwanStationPS5 - a real heap for the PS5 build.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On hardware, ordinary malloc() of a few MB failed ("OOM for vout_buf",
 * SDL textures): the C heap a native title gets is small. SwanStationPS5 therefore
 * reserves a large block of cached direct memory at the first allocation and
 * runs dlmalloc (public domain) on it. The linker's --wrap redirects every
 * malloc/free/... in SwanStationPS5, the core and SDL here (APP_WRAP_SYMBOLS).
 * Memory the system libraries allocated themselves (strdup, fopen buffers)
 * lies outside the block and goes back to the real free/realloc.
 */
#if defined(__PROSPERO__)
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void *mspace;
mspace create_mspace_with_base(void *base, size_t capacity, int locked);
void *mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void *mem);
void *mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void *mspace_realloc(mspace msp, void *mem, size_t newsize);
void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_address, size_t alignment);

void *__real_malloc(size_t size);
void __real_free(void *ptr);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t alignment, size_t size);
int __real_posix_memalign(void **out, size_t alignment, size_t size);

#define MEMORY_TYPE_WB_ONION 0 /* CPU-cached */
#define PROT_CPU_RW 0x3
#define ARENA_ALIGN 0x200000

static mspace heap;
static uint8_t *arena_lo, *arena_hi;
static int init_state; /* 0 not tried, 1 ready, -1 failed: use the system heap */

static void heap_init(void)
{
    if (init_state)
        return;
    init_state = -1;
    static const size_t sizes[] = {(size_t)1 << 30, (size_t)512 << 20, (size_t)256 << 20};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    {
        int64_t physical = 0;
        if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), sizes[i],
                                          ARENA_ALIGN, MEMORY_TYPE_WB_ONION, &physical) < 0)
            continue;
        void *base = NULL;
        if (sceKernelMapDirectMemory(&base, sizes[i], PROT_CPU_RW, 0, physical, ARENA_ALIGN) < 0)
            continue;
        heap = create_mspace_with_base(base, sizes[i], 1);
        if (heap)
        {
            arena_lo = base;
            arena_hi = (uint8_t *)base + sizes[i];
            init_state = 1;
            return;
        }
    }
}

static int ours(const void *p)
{
    return (const uint8_t *)p >= arena_lo && (const uint8_t *)p < arena_hi;
}

void *__wrap_malloc(size_t size)
{
    heap_init();
    void *p = init_state > 0 ? mspace_malloc(heap, size) : NULL;
    return p ? p : __real_malloc(size);
}

void __wrap_free(void *ptr)
{
    if (!ptr)
        return;
    if (ours(ptr))
        mspace_free(heap, ptr);
    else
        __real_free(ptr);
}

void *__wrap_calloc(size_t n, size_t size)
{
    heap_init();
    void *p = init_state > 0 ? mspace_calloc(heap, n, size) : NULL;
    return p ? p : __real_calloc(n, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return __wrap_malloc(size);
    if (ours(ptr))
        return mspace_realloc(heap, ptr, size);
    return __real_realloc(ptr, size); /* allocated by a system library */
}

void *__wrap_memalign(size_t alignment, size_t size)
{
    heap_init();
    void *p = init_state > 0 ? mspace_memalign(heap, alignment, size) : NULL;
    return p ? p : __real_memalign(alignment, size);
}

int __wrap_posix_memalign(void **out, size_t alignment, size_t size)
{
    heap_init();
    void *p = init_state > 0 ? mspace_memalign(heap, alignment, size) : NULL;
    if (p)
    {
        *out = p;
        return 0;
    }
    return __real_posix_memalign(out, alignment, size);
}

void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    return __wrap_memalign(alignment, size);
}

void *__real_reallocf(void *ptr, size_t size);
size_t __real_malloc_usable_size(const void *ptr);
size_t mspace_usable_size(const void *mem);

void *__wrap_reallocf(void *ptr, size_t size)
{
    if (!ptr || ours(ptr))
    {
        void *p = __wrap_realloc(ptr, size);
        if (!p && ptr && size)
            __wrap_free(ptr);
        return p;
    }
    return __real_reallocf(ptr, size);
}

size_t __wrap_malloc_usable_size(const void *ptr)
{
    if (ptr && ours(ptr))
        return mspace_usable_size(ptr);
    return __real_malloc_usable_size(ptr);
}

/* For the log: how big the heap is ("1024 MB", "system" when it fell back). */
size_t ps5_heap_size_mb(void)
{
    heap_init();
    return init_state > 0 ? (size_t)(arena_hi - arena_lo) >> 20 : 0;
}
#endif
