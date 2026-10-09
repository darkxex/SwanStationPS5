/*
 * SwanStationPS5 - single compilation unit for the stb libraries (public domain / MIT).
 *
 * No thread-locals (the PS5 build would need emulated TLS for stbi's error
 * string) and no assert() (the PS5 libc has no __assert).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"
