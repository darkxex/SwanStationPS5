/*
 * SwanStationPS5 desktop build - the console-only features the shared code still calls (games on NFS shares and NTFS
 * disks, the USB disc drive), answered as "none here". The library on PC is the folder of SwanStationPS5_ROOT.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* The PS5 build compiles every .c of src/: this one is the PC's and is empty there (NFS and NTFS are real there). */
#if defined(SwanStationPS5_DESKTOP)

#include "../nfs/nfs.h"
#include "../cdscsi.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- rename over an existing file (see desktop_compat.h) ---- */
#if defined(_WIN32)
#include <windows.h>

/* The names are in the ANSI code page, like the ones fopen and opendir take and give on Windows: through the same
 * (narrow) call, so a game folder with accents is found the same way by all of them. */
int SwanStationPS5_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
        return 0;
    switch (GetLastError())
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        errno = EACCES;
        break;
    default:
        errno = EIO;
    }
    return -1;
}
#endif

/* ---- NFS ---- */
bool nfs_set_shares(const char *list, char *error, size_t error_size)
{
    (void)list;
    if (error && error_size)
        error[0] = '\0';
    return true;
}
int nfs_mount_all(int timeout_ms) { (void)timeout_ms; return 0; }
int nfs_mount_points(char out[][NFS_PATH_MAX], int max) { (void)out, (void)max; return 0; }
void nfs_mount_begin(int timeout_ms) { (void)timeout_ms; }
bool nfs_mount_running(void) { return false; }
bool nfs_mount_finished(void) { return true; }
int nfs_first_failure(char *detail, size_t size)
{
    if (detail && size)
        detail[0] = '\0';
    return NFS_FAIL_NONE;
}
bool nfs_connection_lost(void) { return false; }
void nfs_set_read_timeout(int seconds) { (void)seconds; }
bool nfs_reconnecting(void) { return false; }
int nfs_unmounted(void) { return 0; }
void nfs_set_sbi_dir(const char *dir) { (void)dir; }
void nfs_describe(char *out, size_t size)
{
    if (out && size)
        out[0] = '\0';
}

/* ---- NTFS (ntfsfs.h is C++: the C entry points are declared here) ---- */
int ntfsfs_scan(void) { return 0; }
int ntfsfs_mount_points(char (*out)[512], int max) { (void)out, (void)max; return 0; }
bool ntfsfs_connection_lost(void) { return false; }
int ntfsfs_dirty_count(void) { return 0; }

#else
typedef int SwanStationPS5_pc_only_file; /* not the PC build: nothing here */
#endif /* SwanStationPS5_DESKTOP */
