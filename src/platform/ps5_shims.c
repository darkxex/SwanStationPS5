/*
 * SwanStationPS5 - small stand-ins for symbols the PS5 libraries don't export.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)
#include <stddef.h>

/* libiconv (pulled in by SDL2) expects FreeBSD's MB_CUR_MAX function and
 * nl_langinfo(CODESET). SwanStationPS5 runs in the C locale with UTF-8 text. */
size_t ___mb_cur_max(void)
{
    return 1;
}

char *nl_langinfo(int item)
{
    (void)item;
    return (char *)"UTF-8";
}

/* __builtin_cpu_init/__builtin_cpu_supports normally come from compiler-rt.
 * The cores only use them to warn about a missing SSE2/AVX. The PS5's
 * Zen 2 CPU has CMOV, MMX, POPCNT, SSE..SSE4.2, AVX and AVX2: feature bits
 * 0-10 in compiler-rt's numbering. */
struct ProcessorModel
{
    unsigned int vendor, type, subtype;
    unsigned int features[1];
};

struct ProcessorModel __cpu_model = {2 /* AMD */, 0, 0, {(1u << 11) - 1}};

int __cpu_indicator_init(void)
{
    return 0;
}

/* strcasestr exists on the PS5 only in libScePosixForWebKit, the browser's
 * library: called from a game process (the core checks a path for ".m3u")
 * it crashed with a null access inside the system library. Defining it here
 * keeps the linker from importing that module. */
#include <ctype.h>
char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; ++haystack)
    {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
            ++h, ++n;
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

/* PacBrew's SDL2 supports USB keyboards and the on-screen keyboard through
 * libSceKeyboard and libSceImeDialog. Importing those modules stops the title
 * from launching at all ("Can't start the game or app"), and SwanStationPS5 needs
 * neither. Defining the functions here keeps the linker from importing them;
 * each reports failure, so SDL simply runs without a keyboard. */
#define SCE_UNAVAILABLE ((int)0x80020016) /* generic "not supported" */

int sceKeyboardInit(void) { return SCE_UNAVAILABLE; }
int sceKeyboardOpen(int user, int type, int index, void *param)
{
    (void)user; (void)type; (void)index; (void)param;
    return SCE_UNAVAILABLE;
}
int sceKeyboardClose(int handle) { (void)handle; return SCE_UNAVAILABLE; }
int sceKeyboardReadState(int handle, void *data) { (void)handle; (void)data; return SCE_UNAVAILABLE; }
int sceImeDialogInit(const void *param, void *extended) { (void)param; (void)extended; return SCE_UNAVAILABLE; }
int sceImeDialogGetStatus(void) { return 0; /* SCE_IME_DIALOG_STATUS_NONE */ }
int sceImeDialogGetResult(void *result) { (void)result; return SCE_UNAVAILABLE; }
int sceImeDialogTerm(void) { return SCE_UNAVAILABLE; }

#if defined(SwanStationPS5_VULKAN)
/* RADV (its shader cache) calls dirfd, which the PS5's libc doesn't export.
 * SwanStationPS5 keeps libc's opendir, whose DIR is FreeBSD's struct _dirdesc: the
 * descriptor is its first member. */
#include <dirent.h>
int dirfd(DIR *dir)
{
    return dir ? *(const int *)(const void *)dir : -1;
}
#endif
#endif
