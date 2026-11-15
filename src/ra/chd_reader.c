/*
 * SwanStationPS5 - reads the sectors of a .chd for RetroAchievements' game hash.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * rcheevos asks for a track of the disc and then for its sectors, counted from the start of the track's data (the
 * first sector of the data is 0: the volume descriptor is at 16). A CHD keeps every CD frame as 2352 bytes of sector
 * plus 96 of subcode, tracks padded to a multiple of 4 frames, and may or may not store the pregap.
 */
#include "chd_reader.h"

/* The core keeps its libchdr private except for the few calls the build script exports, renamed swanstation_*. */
#define chd_open swanstation_chd_open
#define chd_close swanstation_chd_close
#define chd_read swanstation_chd_read
#define chd_get_header swanstation_chd_get_header
#define chd_get_metadata swanstation_chd_get_metadata

#include "../SwanStationPS5.h"
#include "rc_hash.h"

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRACKS 99
#define MAX_OPEN 4
#define FRAME_SIZE (2352 + 96)

typedef struct
{
    unsigned number;
    bool audio;
    unsigned user_offset; /* where the 2048 bytes of a sector start inside its 2352 */
    uint32_t frames;      /* of the data (without a pregap that is stored in the file) */
    uint32_t file_frame;  /* of its first sector of data, in the CHD */
} Track;

typedef struct
{
    chd_file *chd;
    uint32_t hunk_bytes, frames_per_hunk;
    uint8_t *hunk;
    uint32_t hunk_index;
    Track track;
} ChdHandle;

static ChdHandle *open_handles[MAX_OPEN];
static rc_hash_cdreader_t fallback; /* rcheevos' own reader, for everything that is not a .chd */

static bool is_ours(void *handle)
{
    for (int i = 0; i < MAX_OPEN; ++i)
        if (open_handles[i] && open_handles[i] == handle)
            return true;
    return false;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    return p;
}

static const char *match(const char *p, const char *lit)
{
    p = skip_ws(p);
    const size_t n = strlen(lit);
    return strncmp(p, lit, n) == 0 ? p + n : NULL;
}

static const char *parse_int(const char *p, int *out)
{
    p = skip_ws(p);
    char *end = NULL;
    const long v = strtol(p, &end, 10);
    if (end == p)
        return NULL;
    *out = (int)v;
    return end;
}

static const char *parse_token(const char *p, char *dst, size_t size)
{
    p = skip_ws(p);
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        ++p;
    if (p == start)
        return NULL;
    size_t n = (size_t)(p - start);
    if (n >= size)
        n = size - 1;
    memcpy(dst, start, n);
    dst[n] = '\0';
    return p;
}

/* "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:12345 [PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0]" */
static bool parse_track(const char *s, bool v2, int *number, char *type, size_t type_size, int *frames, int *pregap,
                        char *pgtype, size_t pgtype_size)
{
    char sub[32], pgsub[32];
    const char *p = s;
    *pregap = 0;
    pgtype[0] = '\0';
    if (!(p = match(p, "TRACK:")) || !(p = parse_int(p, number)) || !(p = match(p, "TYPE:")) ||
        !(p = parse_token(p, type, type_size)) || !(p = match(p, "SUBTYPE:")) || !(p = parse_token(p, sub, sizeof(sub))) ||
        !(p = match(p, "FRAMES:")) || !(p = parse_int(p, frames)))
        return false;
    if (!v2)
        return true;
    int postgap;
    return (p = match(p, "PREGAP:")) && (p = parse_int(p, pregap)) && (p = match(p, "PGTYPE:")) &&
           (p = parse_token(p, pgtype, pgtype_size)) && (p = match(p, "PGSUB:")) && (p = parse_token(p, pgsub, sizeof(pgsub))) &&
           (p = match(p, "POSTGAP:")) && parse_int(p, &postgap);
}

static bool is_chd(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot)
        return false;
    ++dot;
    return (dot[0] | 0x20) == 'c' && (dot[1] | 0x20) == 'h' && (dot[2] | 0x20) == 'd' && !dot[3];
}

/* Finds the track rcheevos wants (a number, or RC_HASH_CDTRACK_FIRST_DATA / LAST / LARGEST). */
static bool find_track(chd_file *chd, uint32_t want, Track *out)
{
    Track tracks[MAX_TRACKS];
    int count = 0;
    uint32_t file_frame = 0;
    for (; count < MAX_TRACKS; ++count)
    {
        char meta[256], type[64], pgtype[64];
        uint32_t len;
        int number = 0, frames = 0, pregap = 0;
        bool v2 = true;
        if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, (uint32_t)count, meta, sizeof(meta), &len, NULL, NULL) !=
            CHDERR_NONE)
        {
            v2 = false;
            if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, (uint32_t)count, meta, sizeof(meta), &len, NULL, NULL) !=
                CHDERR_NONE)
                break;
        }
        if (!parse_track(meta, v2, &number, type, sizeof(type), &frames, &pregap, pgtype, sizeof(pgtype)))
            return false;
        Track *t = &tracks[count];
        t->number = (unsigned)number;
        t->audio = strcmp(type, "AUDIO") == 0;
        t->user_offset = strcmp(type, "MODE1_RAW") == 0                                                 ? 16
                         : strcmp(type, "MODE2_RAW") == 0                                               ? 24
                         : (strcmp(type, "MODE2") == 0 || strcmp(type, "MODE2_FORM_MIX") == 0)          ? 8
                                                                                                         : 0;
        /* a pregap kept in the file ('V...') comes before the data of the track */
        const uint32_t stored_pregap = (pregap > 0 && pgtype[0] == 'V') ? (uint32_t)pregap : 0;
        t->frames = (uint32_t)frames - stored_pregap;
        t->file_frame = file_frame + stored_pregap;
        file_frame += (uint32_t)frames;
        file_frame = (file_frame + (CD_TRACK_PADDING - 1)) & ~(uint32_t)(CD_TRACK_PADDING - 1);
    }
    if (!count)
        return false;
    int pick = -1;
    if (want == RC_HASH_CDTRACK_FIRST_DATA)
    {
        for (int i = 0; i < count && pick < 0; ++i)
            if (!tracks[i].audio)
                pick = i;
    }
    else if (want == RC_HASH_CDTRACK_LAST)
        pick = count - 1;
    else if (want == RC_HASH_CDTRACK_LARGEST)
    {
        pick = 0;
        for (int i = 1; i < count; ++i)
            if (tracks[i].frames > tracks[pick].frames)
                pick = i;
    }
    else if (want != RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION)
    {
        for (int i = 0; i < count && pick < 0; ++i)
            if (tracks[i].number == want)
                pick = i;
    }
    if (pick < 0)
        return false;
    *out = tracks[pick];
    return true;
}

static void *RC_CCONV chd_open_track(const char *path, uint32_t track, const struct rc_hash_iterator *iterator)
{
    if (!is_chd(path))
        return fallback.open_track_iterator ? fallback.open_track_iterator(path, track, iterator) : NULL;

    chd_file *chd = NULL;
    if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE || !chd)
        return NULL;
    const chd_header *hdr = chd_get_header(chd);
    ChdHandle *h = hdr && hdr->hunkbytes && hdr->hunkbytes % FRAME_SIZE == 0 ? calloc(1, sizeof(*h)) : NULL;
    int slot = -1;
    for (int i = 0; i < MAX_OPEN && slot < 0; ++i)
        if (!open_handles[i])
            slot = i;
    if (!h || slot < 0 || !find_track(chd, track, &h->track))
    {
        free(h);
        chd_close(chd);
        return NULL;
    }
    h->chd = chd;
    h->hunk_bytes = hdr->hunkbytes;
    h->frames_per_hunk = hdr->hunkbytes / FRAME_SIZE;
    h->hunk = malloc(hdr->hunkbytes);
    h->hunk_index = (uint32_t)-1;
    if (!h->hunk)
    {
        free(h);
        chd_close(chd);
        return NULL;
    }
    open_handles[slot] = h;
    return h;
}

static size_t RC_CCONV chd_read_sector(void *handle, uint32_t sector, void *buffer, size_t requested)
{
    if (!is_ours(handle))
        return fallback.read_sector ? fallback.read_sector(handle, sector, buffer, requested) : 0;
    ChdHandle *h = handle;
    uint8_t *out = buffer;
    size_t total = 0;
    while (requested)
    {
        if (sector >= h->track.frames)
            break;
        const uint32_t frame = h->track.file_frame + sector;
        const uint32_t hunk = frame / h->frames_per_hunk;
        if (hunk != h->hunk_index)
        {
            if (chd_read(h->chd, hunk, h->hunk) != CHDERR_NONE)
                break;
            h->hunk_index = hunk;
        }
        const uint8_t *data = h->hunk + (frame % h->frames_per_hunk) * FRAME_SIZE + h->track.user_offset;
        const size_t n = requested < 2048 ? requested : 2048;
        memcpy(out, data, n);
        out += n;
        total += n;
        requested -= n;
        ++sector;
    }
    return total;
}

static void RC_CCONV chd_close_track(void *handle)
{
    if (!is_ours(handle))
    {
        if (fallback.close_track)
            fallback.close_track(handle);
        return;
    }
    ChdHandle *h = handle;
    for (int i = 0; i < MAX_OPEN; ++i)
        if (open_handles[i] == h)
            open_handles[i] = NULL;
    chd_close(h->chd);
    free(h->hunk);
    free(h);
}

static uint32_t RC_CCONV chd_first_track_sector(void *handle)
{
    if (!is_ours(handle))
        return fallback.first_track_sector ? fallback.first_track_sector(handle) : 0;
    return 0; /* sectors are counted from the start of the track's data */
}

void ra_chd_reader_init(void)
{
    rc_hash_get_default_cdreader(&fallback);
    rc_hash_cdreader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.open_track = NULL;
    reader.open_track_iterator = chd_open_track;
    reader.read_sector = chd_read_sector;
    reader.close_track = chd_close_track;
    reader.first_track_sector = chd_first_track_sector;
    rc_hash_init_custom_cdreader(&reader);
}
