/*
 * SwanStationPS5 - reads a game's serial (SLUS-01041 ...) from its disc image.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every PS1 disc has SYSTEM.CNF in its root with "BOOT = cdrom:\SLUS_010.41;1".
 * That serial is region-exact, so covers are matched by it.
 */
#include "disc.h"

#include "core/host.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int prefix_icmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d || !a[i])
            return d;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool disc_format_serial(const char *raw, char *serial, size_t size)
{
    for (const char *p = raw; p[0] && p[1] && p[2] && p[3]; ++p)
    {
        if (!(isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) &&
              isalpha((unsigned char)p[2]) && isalpha((unsigned char)p[3])))
            continue;
        char digits[6];
        int n = 0;
        const char *q = p + 4;
        if (*q == '_' || *q == '-')
            ++q;
        while (*q && n < 5)
        {
            if (isdigit((unsigned char)*q))
                digits[n++] = *q;
            else if (*q != '.')
                break;
            ++q;
        }
        if (n != 5)
            continue;
        digits[5] = '\0';
        snprintf(serial, size, "%c%c%c%c-%s", toupper((unsigned char)p[0]),
                 toupper((unsigned char)p[1]), toupper((unsigned char)p[2]),
                 toupper((unsigned char)p[3]), digits);
        return true;
    }
    return false;
}

typedef struct
{
    FILE *f;
    long sector, offset;
} Image;

static bool image_open(Image *img, const char *path)
{
    img->f = fopen(path, "rb");
    if (!img->f)
        return false;
    /* raw Mode 2, raw Mode 1, cooked, raw with subchannel (2448), Mode 2 without sync (2336) */
    static const long layouts[][2] = {{2352, 24}, {2352, 16}, {2048, 0}, {2448, 24}, {2336, 8}};
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
    {
        uint8_t id[6];
        fseek(img->f, 16 * layouts[i][0] + layouts[i][1], SEEK_SET);
        if (fread(id, 1, 6, img->f) == 6 && memcmp(id + 1, "CD001", 5) == 0)
        {
            img->sector = layouts[i][0];
            img->offset = layouts[i][1];
            return true;
        }
    }
    fclose(img->f);
    return false;
}

static bool image_read(Image *img, uint32_t lba, uint8_t *out, size_t length)
{
    while (length > 0)
    {
        size_t chunk = length < 2048 ? length : 2048;
        if (fseek(img->f, (long)lba * img->sector + img->offset, SEEK_SET) != 0 ||
            fread(out, 1, chunk, img->f) != chunk)
            return false;
        out += chunk;
        length -= chunk;
        ++lba;
    }
    return true;
}

/* sectors from a plain image file */
typedef bool (*ReadSectors)(void *ctx, uint32_t lba, uint8_t *out, size_t length);

static bool read_image(void *ctx, uint32_t lba, uint8_t *out, size_t length)
{
    return image_read(ctx, lba, out, length);
}

static bool find_serial(ReadSectors read, void *ctx, char *serial, size_t size)
{
    bool ok = false;
    uint8_t pvd[2048];
    static uint8_t root[16384];
    if (read(ctx, 16, pvd, sizeof(pvd)) && memcmp(pvd + 1, "CD001", 5) == 0)
    {
        uint32_t root_lba = le32(pvd + 158), root_size = le32(pvd + 166);
        if (root_size > sizeof(root))
            root_size = sizeof(root);
        root_size -= root_size % 2048;
        if (root_size && read(ctx, root_lba, root, root_size))
        {
            for (uint32_t pos = 0; pos < root_size;)
            {
                uint8_t len = root[pos];
                if (len == 0)
                {
                    pos = (pos / 2048 + 1) * 2048; /* records never span sectors */
                    continue;
                }
                uint8_t name_len = root[pos + 32];
                if (name_len >= 10 && strncmp((const char *)root + pos + 33, "SYSTEM.CNF", 10) == 0)
                {
                    char cnf[2049] = {0};
                    uint32_t cnf_size = le32(root + pos + 10);
                    if (cnf_size > 2048)
                        cnf_size = 2048;
                    if (read(ctx, le32(root + pos + 2), (uint8_t *)cnf, 2048))
                    {
                        cnf[cnf_size] = '\0';
                        const char *boot = strstr(cnf, "BOOT");
                        ok = boot && disc_format_serial(boot, serial, size);
                    }
                    break;
                }
                pos += len;
            }
        }
    }
    return ok;
}

static bool read_iso(const char *path, char *serial, size_t size)
{
    Image img;
    if (!image_open(&img, path))
        return false;
    bool ok = find_serial(read_image, &img, serial, size);
    fclose(img.f);
    return ok;
}

static bool read_pbp(const char *path, char *serial, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t head[40];
    bool ok = false;
    if (fread(head, 1, 40, f) == 40 && memcmp(head, "\0PBP", 4) == 0)
    {
        uint32_t sfo_off = le32(head + 8), sfo_end = le32(head + 12);
        uint32_t sfo_len = sfo_end > sfo_off ? sfo_end - sfo_off : 0;
        uint8_t *sfo = (sfo_len > 20 && sfo_len < 65536) ? malloc(sfo_len) : NULL;
        if (sfo && fseek(f, (long)sfo_off, SEEK_SET) == 0 && fread(sfo, 1, sfo_len, f) == sfo_len)
        {
            uint32_t keys = le32(sfo + 8), data = le32(sfo + 12), count = le32(sfo + 16);
            for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= sfo_len; ++i)
            {
                const uint8_t *e = sfo + 20 + i * 16;
                uint32_t k = keys + (e[0] | e[1] << 8), d = data + le32(e + 12);
                if (k < sfo_len && strcmp((const char *)sfo + k, "DISC_ID") == 0 && d < sfo_len)
                {
                    char id[32] = {0};
                    memcpy(id, sfo + d, d + 16 <= sfo_len ? 16 : sfo_len - d);
                    ok = disc_format_serial(id, serial, size);
                    break;
                }
            }
        }
        free(sfo);
    }
    fclose(f);
    return ok;
}

/* First FILE "..." of a cue sheet, or first entry of an m3u, next to `path`. */
static bool first_referenced(const char *path, bool cue, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[512], name[512] = "";
    while (fgets(line, sizeof(line), f) && !name[0])
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (cue)
        {
            /* FILE "name.bin" BINARY, or FILE name.bin BINARY without quotes */
            char *q = line;
            while (*q == ' ' || *q == '\t' || (unsigned char)*q == 0xef || (unsigned char)*q == 0xbb ||
                   (unsigned char)*q == 0xbf)
                ++q;
            if (prefix_icmp(q, "FILE", 4) != 0 || (q[4] != ' ' && q[4] != '\t'))
                continue;
            q += 5;
            while (*q == ' ' || *q == '\t')
                ++q;
            char *a = *q == '"' ? q : NULL;
            char *b = a ? strchr(a + 1, '"') : NULL;
            if (a && b)
            {
                *b = '\0';
                str_copy(name, sizeof(name), a + 1);
            }
            else
            {
                char *type = strrchr(q, ' '); /* the name runs up to the type (BINARY) */
                if (type && type > q)
                    *type = '\0';
                str_copy(name, sizeof(name), q);
            }
        }
        else if (line[0] && line[0] != '#')
            str_copy(name, sizeof(name), line);
    }
    fclose(f);
    if (!name[0])
        return false;
    char dir[SwanStationPS5_PATH_MAX];
    str_copy(dir, sizeof(dir), path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        str_copy(dir, sizeof(dir), ".");
    path_join(out, size, dir, name);
    return true;
}

bool disc_read_serial(const char *path, char *serial, size_t size)
{
    const char *ext = path_ext(path);
    char next[SwanStationPS5_PATH_MAX];
    if (str_icmp(ext, "m3u") == 0)
        return first_referenced(path, false, next, sizeof(next)) &&
               disc_read_serial(next, serial, size);
    if (str_icmp(ext, "cue") == 0)
    {
        if (first_referenced(path, true, next, sizeof(next)) && disc_read_serial(next, serial, size))
            return true;
        /* the cue names a file that isn't there as written (a different case, a
         * renamed .bin): try a .bin of the same name, then the folder's only one */
        char dir[SwanStationPS5_PATH_MAX], base[256];
        str_copy(dir, sizeof(dir), path);
        char *slash = strrchr(dir, '/');
        if (!slash)
            return false;
        *slash = '\0';
        str_copy(base, sizeof(base), slash + 1);
        char *dot = strrchr(base, '.');
        if (dot)
            *dot = '\0';
        DIR *d = opendir(dir);
        if (!d)
            return false;
        char same[SwanStationPS5_PATH_MAX] = "", only[SwanStationPS5_PATH_MAX] = "";
        int bins = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (str_icmp(path_ext(e->d_name), "bin") != 0)
                continue;
            ++bins;
            path_join(only, sizeof(only), dir, e->d_name);
            size_t n = strlen(base);
            if (!same[0] && prefix_icmp(e->d_name, base, n) == 0 && e->d_name[n] == '.')
                str_copy(same, sizeof(same), only);
        }
        closedir(d);
        if (same[0] && read_iso(same, serial, size))
            return true;
        return bins == 1 && read_iso(only, serial, size);
    }
    if (str_icmp(ext, "pbp") == 0)
        return read_pbp(path, serial, size);
    if (str_icmp(ext, "bin") == 0 || str_icmp(ext, "iso") == 0 || str_icmp(ext, "img") == 0 ||
        str_icmp(ext, "mdf") == 0)
        return read_iso(path, serial, size);
    if (str_icmp(ext, "ccd") == 0)
    {
        /* CloneCD: the sectors are in the .img of the same name */
        str_copy(next, sizeof(next), path);
        char *dot = strrchr(next, '.');
        if (dot && (size_t)(dot - next) + 5 < sizeof(next))
            strcpy(dot, ".img");
        return read_iso(next, serial, size);
    }
    return false; /* .chd and the rest: SwanStation reads them, the serial is not known */
}
