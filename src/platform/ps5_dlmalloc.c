/*
 * SwanStationPS5 - dlmalloc 2.8.6 (public domain) built as a single mspace for
 * ps5_heap.c: no system memory of its own, thread-safe spin locks.
 */
#if defined(__PROSPERO__)
#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define LACKS_SYS_MMAN_H 1
#define NO_MALLOC_STATS 1
#define NO_MALLINFO 1
#define malloc_getpagesize ((size_t)16384U)
/* The PS5 compiler assumes operator new returns 32-byte aligned memory
 * (__STDCPP_DEFAULT_NEW_ALIGNMENT__) and zeroes new objects with 32-byte
 * AVX stores (vmovaps ymm): 16 crashed SPIRV-Cross. */
#define MALLOC_ALIGNMENT ((size_t)32U)
#include "../../third_party/dlmalloc/malloc.c"
#endif
