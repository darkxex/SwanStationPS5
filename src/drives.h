#ifndef SwanStationPS5_DRIVES_H
#define SwanStationPS5_DRIVES_H

#define DRIVE_EXT_COUNT 2
#define DRIVE_USB_COUNT 8
#define DRIVE_COUNT (DRIVE_EXT_COUNT + DRIVE_USB_COUNT)

static const char *const drive_paths[DRIVE_COUNT] = {"/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2",
                                                     "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"};


#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(_WIN32)
/* PC build: no /mnt drives (the library lives in SwanStationPS5_ROOT) */
#elif defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
extern int drives_fstatfs(int, struct statfs *) __asm__("_fstatfs");
#endif

static inline long long drive_free_mb(const char *path)
{
#if defined(_WIN32)
    (void)path;
    return -1;
}
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    struct statfs fs;
    memset(&fs, 0, sizeof(fs));
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -1;
#ifdef __linux__
    int measured = fstatfs(fd, &fs);
#else
    int measured = drives_fstatfs(fd, &fs);
#endif
    close(fd);
    return measured == 0 ? (long long)((unsigned long long)fs.f_bavail * fs.f_bsize >> 20) : -1;
}
#endif

static inline bool drive_usable(int d)
{
    return drive_free_mb(drive_paths[d]) >= 256;
}

static inline int drives_list(char *buf, size_t size, int only)
{
    int found = 0;
    size_t len = 0;
    if (size)
        buf[0] = '\0';
    for (int d = 0; d < DRIVE_COUNT; ++d)
    {
        if (!drive_usable(d))
            continue;
        long long free_mb = drive_free_mb(drive_paths[d]);
        if (only >= 0 && found++ != only)
            continue;
        const char *name = drive_paths[d] + 5;
        int n = snprintf(buf + len, size - len, "%s%s %lld.%lld GB", only < 0 && len ? ", " : "", name, free_mb >> 10,
                         (free_mb & 1023) * 10 >> 10);
        if (n < 0 || (size_t)n >= size - len)
            break;
        len += (size_t)n;
        if (only >= 0)
            break;
        ++found;
    }
    return found;
}

static inline bool drives_ext_games(char *out, size_t size)
{
    for (int d = 0; d < DRIVE_EXT_COUNT; ++d)
        if (drive_free_mb(drive_paths[d]) >= 1024)
        {
            char base[64];
            snprintf(base, sizeof(base), "%s/SwanStationPS5", drive_paths[d]);
            snprintf(out, size, "%s/games", base);
            mkdir(base, 0777);
            mkdir(out, 0777);
            return true;
        }
    return false;
}

#endif
