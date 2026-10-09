/*
 * PSXS5 - game library scanning.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "library.h"

#include "disc.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIR_FILES 64
#define MAX_ROOT_ENTRIES 4096 /* entries read from the games folder (static: 1 MB of names) */

/* Loadable images, best first: a playlist beats a single disc. */
static int image_rank(const char *name)
{
    const char *ext = path_ext(name);
    if (str_icmp(ext, "m3u") == 0)
        return 6;
    if (str_icmp(ext, "pbp") == 0)
        return 5;
    if (str_icmp(ext, "chd") == 0)
        return 4;
    if (str_icmp(ext, "cue") == 0 || str_icmp(ext, "ccd") == 0)
        return 3;
    if (str_icmp(ext, "iso") == 0 || str_icmp(ext, "img") == 0 || str_icmp(ext, "mdf") == 0)
        return 2;
    if (str_icmp(ext, "bin") == 0)
        return 1; /* only used when no sheet describes it */
    return 0;
}

static bool contains_icase(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; ++haystack)
    {
        size_t i = 0;
        while (i < n && haystack[i] &&
               tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == n)
            return true;
    }
    return false;
}

static void extract_serial(const char *name, char *serial, size_t size)
{
    static const char *prefixes[] = {"SLUS", "SCUS", "SLES", "SCES", "SLPS", "SCPS",
                                     "SLPM", "SIPS", "SLKA", "PAPX", "SCED"};
    serial[0] = '\0';
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p)
    {
        const char *hit = strstr(name, prefixes[p]);
        if (!hit)
            continue;
        size_t n = 4;
        while (n < 12 && (isdigit((unsigned char)hit[n]) || hit[n] == '-' || hit[n] == '_' ||
                          hit[n] == '.'))
            ++n;
        if (n > 6)
        {
            str_copy(serial, size < n + 1 ? size : n + 1, hit);
            return;
        }
    }
}

/* "Resident Evil 2 [U] [SLUS-00421]" -> "Resident Evil 2"
 * "Chrono Cross (USA) (Disc 1)"      -> "Chrono Cross (USA)" */
static void clean_title(const char *raw, char *out, size_t size)
{
    char buf[256];
    str_copy(buf, sizeof(buf), raw);
    char *dot = strrchr(buf, '.');
    if (dot && image_rank(buf) > 0)
        *dot = '\0';

    char result[256];
    size_t w = 0;
    for (const char *p = buf; *p && w + 1 < sizeof(result);)
    {
        if (*p == '[')
        {
            const char *end = strchr(p, ']');
            if (end)
            {
                p = end + 1;
                continue;
            }
        }
        if (*p == '(' && (strncmp(p, "(Disc", 5) == 0 || strncmp(p, "(disc", 5) == 0 ||
                          strncmp(p, "(CD", 3) == 0 || strncmp(p, "(PSXS5)", 7) == 0))
        {
            const char *end = strchr(p, ')');
            if (end)
            {
                p = end + 1;
                continue;
            }
        }
        result[w++] = (*p == '_') ? ' ' : *p;
        ++p;
    }
    result[w] = '\0';

    /* collapse whitespace */
    char *src = result, *dst = result;
    bool space = true;
    for (; *src; ++src)
    {
        if (isspace((unsigned char)*src))
        {
            if (!space)
                *dst++ = ' ';
            space = true;
        }
        else
        {
            *dst++ = *src;
            space = false;
        }
    }
    while (dst > result && dst[-1] == ' ')
        --dst;
    *dst = '\0';
    str_copy(out, size, result[0] ? result : raw);
}

/* Serial from the disc itself (exact), else serial.txt written by the sync
 * tool (needed for .chd), else whatever the file name carried. Then the id. */
static void finish_game(Game *g)
{
    char serial[16];
    char txt[PSXS5_PATH_MAX];
    if (disc_read_serial(g->path, serial, sizeof(serial)))
        str_copy(g->serial, sizeof(g->serial), serial);
    else
    {
        path_join(txt, sizeof(txt), g->folder, "serial.txt");
        FILE *f = fopen(txt, "r");
        if (f)
        {
            char line[64] = "";
            if (fgets(line, sizeof(line), f) && disc_format_serial(line, serial, sizeof(serial)))
                str_copy(g->serial, sizeof(g->serial), serial);
            fclose(f);
        }
    }

    if (g->serial[0])
    {
        str_copy(g->id, sizeof(g->id), g->serial);
        return;
    }
    psxs5_log("library: no serial found in %s (put it in the file or folder name, or a serial.txt beside it)",
              g->path);
    size_t w = 0;
    for (const char *p = g->title; *p && w + 1 < sizeof(g->id); ++p)
        g->id[w++] = isalnum((unsigned char)*p) ? *p : '_';
    g->id[w] = '\0';
}

static Game *add_game(Library *lib)
{
    if (lib->count == lib->capacity)
    {
        int cap = lib->capacity ? lib->capacity * 2 : 64;
        Game *grown = realloc(lib->games, (size_t)cap * sizeof(Game));
        if (!grown)
            return NULL;
        lib->games = grown;
        lib->capacity = cap;
    }
    Game *g = &lib->games[lib->count++];
    memset(g, 0, sizeof(*g));
    return g;
}

static void set_disc_name(Game *g, const char *file)
{
    str_copy(g->disc_name, sizeof(g->disc_name), file);
    char *dot = strrchr(g->disc_name, '.');
    if (dot)
        *dot = '\0';
}

static int cmp_names(const void *a, const void *b)
{
    return str_icmp((const char *)a, (const char *)b);
}

typedef char NameBuf[256];

/* Reads up to MAX_DIR_FILES loadable entries of `dir`, sorted by name. */
static int list_images(const char *dir, NameBuf *names)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_DIR_FILES)
    {
        if (e->d_name[0] == '.' || image_rank(e->d_name) == 0)
            continue;
        str_copy(names[n++], sizeof(NameBuf), e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(NameBuf), cmp_names);
    return n;
}

static bool write_m3u(const char *path, NameBuf *discs, int count)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    for (int i = 0; i < count; ++i)
        fprintf(f, "%s\n", discs[i]);
    return fclose(f) == 0;
}

/* One folder = one game. */
static void scan_game_folder(Library *lib, const char *dir, const char *folder_name)
{
    static NameBuf names[MAX_DIR_FILES];
    static NameBuf discs[MAX_DIR_FILES];
    int n = list_images(dir, names);
    if (n == 0)
        return;

    int best = -1, best_rank = 0;
    bool has_sheet = false;
    int disc_count = 0;
    for (int i = 0; i < n; ++i)
    {
        int rank = image_rank(names[i]);
        if (rank == 3)
            has_sheet = true;
        if (rank > best_rank)
        {
            best_rank = rank;
            best = i;
        }
    }
    /* Collect disc images of the winning kind (cue/chd/pbp...). */
    for (int i = 0; i < n; ++i)
        if (image_rank(names[i]) == best_rank && best_rank != 6)
            str_copy(discs[disc_count++], sizeof(NameBuf), names[i]);
    if (best_rank == 1 && has_sheet)
        return;

    Game *g = add_game(lib);
    if (!g)
        return;
    clean_title(folder_name, g->title, sizeof(g->title));
    extract_serial(folder_name, g->serial, sizeof(g->serial));
    if (!g->serial[0])
        extract_serial(names[best], g->serial, sizeof(g->serial));
    g->discs = 1;

    if (best_rank == 6)
    {
        path_join(g->path, sizeof(g->path), dir, names[best]);
        /* count lines for the disc indicator */
        FILE *f = fopen(g->path, "r");
        if (f)
        {
            char line[512];
            int lines = 0;
            while (fgets(line, sizeof(line), f))
                if (line[0] && line[0] != '#' && line[0] != '\n' && line[0] != '\r')
                    ++lines;
            fclose(f);
            g->discs = lines > 0 ? lines : 1;
        }
    }
    else if (disc_count > 1 && (contains_icase(discs[0], "disc") || contains_icase(discs[0], "cd")))
    {
        char m3u[PSXS5_PATH_MAX];
        char file[160];
        snprintf(file, sizeof(file), "%.150s.m3u", g->title);
        path_join(m3u, sizeof(m3u), dir, file);
        if (write_m3u(m3u, discs, disc_count))
        {
            str_copy(g->path, sizeof(g->path), m3u);
            g->discs = disc_count;
            psxs5_log("library: wrote %s (%d discs)", m3u, disc_count);
        }
        else
            path_join(g->path, sizeof(g->path), dir, discs[0]);
    }
    else
        path_join(g->path, sizeof(g->path), dir, names[best]);

    str_copy(g->folder, sizeof(g->folder), dir);
    set_disc_name(g, disc_count > 0 ? discs[0] : names[best]);
    finish_game(g);
}

static void scan_root(Library *lib, const char *root)
{
    DIR *d = opendir(root);
    if (!d)
        return;
    static NameBuf entries[MAX_ROOT_ENTRIES];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_ROOT_ENTRIES)
    {
        if (e->d_name[0] == '.')
            continue;
        str_copy(entries[n++], sizeof(NameBuf), e->d_name);
    }
    closedir(d);
    qsort(entries, (size_t)n, sizeof(NameBuf), cmp_names);

    bool root_has_sheet = false;
    for (int i = 0; i < n; ++i)
        if (image_rank(entries[i]) == 3)
            root_has_sheet = true;

    /* Multi-disc games lying loose here ("FF VII (Disc 1).cue", "(Disc 2).cue"...)
     * are one game, as in their own folder: the discs a playlist (.m3u) here
     * already lists are skipped, the others get one written, and the shelf
     * shows the game once, from disc 1. */
    static bool skip[MAX_ROOT_ENTRIES];
    memset(skip, 0, sizeof(skip));
    for (int i = 0; i < n; ++i)
        if (image_rank(entries[i]) == 6)
        {
            /* a playlist made elsewhere (VLC...) may hold a PC's paths: it counts
             * only when every disc it lists is here */
            char m3u[PSXS5_PATH_MAX], line[300];
            path_join(m3u, sizeof(m3u), root, entries[i]);
            FILE *f = fopen(m3u, "r");
            int listed[MAX_DIR_FILES], count = 0;
            bool valid = f != NULL;
            while (f && fgets(line, sizeof(line), f))
            {
                line[strcspn(line, "\r\n")] = '\0';
                if (!line[0] || line[0] == '#')
                    continue;
                int found = -1;
                for (int k = 0; k < n && found < 0; ++k)
                    if (!str_icmp(entries[k], line))
                        found = k;
                if (found < 0)
                    valid = false;
                else if (count < MAX_DIR_FILES)
                    listed[count++] = found;
            }
            if (f)
                fclose(f);
            if (valid && count > 0)
                for (int k = 0; k < count; ++k)
                    skip[listed[k]] = true;
            else
            {
                skip[i] = true; /* not usable here: PSXS5 groups the discs itself */
                psxs5_log("library: %s lists discs that aren't beside it, ignored", m3u);
            }
        }
    for (int i = 0; i < n; ++i)
    {
        int rank = image_rank(entries[i]);
        if (skip[i] || rank < 2 || rank == 6 ||
            !(contains_icase(entries[i], "(disc") || contains_icase(entries[i], "(cd")))
            continue;
        char title[160], other[160];
        clean_title(entries[i], title, sizeof(title));
        static NameBuf discs[MAX_DIR_FILES];
        int count = 0, members[MAX_DIR_FILES];
        for (int k = i; k < n && count < MAX_DIR_FILES; ++k)
        {
            if (skip[k] || image_rank(entries[k]) != rank ||
                !(contains_icase(entries[k], "(disc") || contains_icase(entries[k], "(cd")))
                continue;
            clean_title(entries[k], other, sizeof(other));
            if (str_icmp(title, other) != 0)
                continue;
            members[count] = k;
            str_copy(discs[count++], sizeof(NameBuf), entries[k]);
        }
        if (count < 2)
            continue;
        for (int k = 0; k < count; ++k)
            skip[members[k]] = true;
        Game *g = add_game(lib);
        if (!g)
            return;
        str_copy(g->title, sizeof(g->title), title);
        extract_serial(discs[0], g->serial, sizeof(g->serial));
        str_copy(g->folder, sizeof(g->folder), root);
        char m3u_name[200], m3u[PSXS5_PATH_MAX];
        snprintf(m3u_name, sizeof(m3u_name), "%.190s.m3u", title);
        path_join(m3u, sizeof(m3u), root, m3u_name);
        if (path_exists(m3u))
        {
            /* that name is taken by a playlist that didn't work here: keep it */
            snprintf(m3u_name, sizeof(m3u_name), "%.180s (PSXS5).m3u", title);
            path_join(m3u, sizeof(m3u), root, m3u_name);
        }
        char first[PSXS5_PATH_MAX];
        path_join(first, sizeof(first), root, discs[0]);
        if (write_m3u(m3u, discs, count))
        {
            str_copy(g->path, sizeof(g->path), m3u);
            psxs5_log("library: wrote %s (%d discs)", m3u, count);
        }
        else
            str_copy(g->path, sizeof(g->path), first);
        g->discs = count;
        set_disc_name(g, discs[0]);
        finish_game(g);
    }

    for (int i = 0; i < n; ++i)
    {
        if (skip[i])
            continue;
        char full[PSXS5_PATH_MAX];
        path_join(full, sizeof(full), root, entries[i]);
        if (path_is_dir(full))
        {
            scan_game_folder(lib, full, entries[i]);
            continue;
        }
        int rank = image_rank(entries[i]);
        if (rank == 0 || (rank == 1 && root_has_sheet))
            continue;
        Game *g = add_game(lib);
        if (!g)
            return;
        clean_title(entries[i], g->title, sizeof(g->title));
        extract_serial(entries[i], g->serial, sizeof(g->serial));
        str_copy(g->path, sizeof(g->path), full);
        str_copy(g->folder, sizeof(g->folder), root);
        set_disc_name(g, entries[i]);
        g->discs = 1;
        if (rank == 6)
        {
            /* a playlist: one disc per line */
            FILE *f = fopen(full, "r");
            char line[300];
            int lines = 0;
            while (f && fgets(line, sizeof(line), f))
                lines += line[0] && line[0] != '\n' && line[0] != '\r' && line[0] != '#';
            if (f)
                fclose(f);
            g->discs = lines > 0 ? lines : 1;
        }
        finish_game(g);
    }
}

static int cmp_games(const void *a, const void *b)
{
    return str_icmp(((const Game *)a)->title, ((const Game *)b)->title);
}

void library_scan(Library *lib, const char *const *roots, int root_count)
{
    lib->count = 0;
    for (int i = 0; i < root_count; ++i)
        if (path_is_dir(roots[i]))
            scan_root(lib, roots[i]);
    if (lib->count > 1)
        qsort(lib->games, (size_t)lib->count, sizeof(Game), cmp_games);
    psxs5_log("library: %d games", lib->count);
}

bool library_load_index(Library *lib, const char *index_path)
{
    FILE *f = fopen(index_path, "r");
    if (!f)
        return false;
    lib->count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#')
            continue;
        char *field[5] = {0};
        char *p = line;
        for (int i = 0; i < 5 && p; ++i)
        {
            field[i] = p;
            p = strchr(p, '\t');
            if (p)
                *p++ = '\0';
        }
        if (!field[3] || !field[3][0])
            continue;
        Game *g = add_game(lib);
        if (!g)
            break;
        clean_title(field[0][0] ? field[0] : "Game", g->title, sizeof(g->title));
        str_copy(g->serial, sizeof(g->serial), field[1] ? field[1] : "");
        g->discs = field[2] ? atoi(field[2]) : 1;
        if (g->discs < 1)
            g->discs = 1;
        str_copy(g->path, sizeof(g->path), field[3]);
        str_copy(g->folder, sizeof(g->folder), field[3]);
        char *slash = strrchr(g->folder, '/');
        if (slash)
            *slash = '\0';
        /* already without extension; don't cut names like "Dr. Mario" at the dot */
        str_copy(g->disc_name, sizeof(g->disc_name), field[4] && field[4][0] ? field[4] : g->title);
        if (g->serial[0])
            str_copy(g->id, sizeof(g->id), g->serial);
        else
            finish_game(g); /* derive an id from the title */
    }
    fclose(f);
    if (lib->count > 1)
        qsort(lib->games, (size_t)lib->count, sizeof(Game), cmp_games);
    psxs5_log("library: %d games from index %s", lib->count, index_path);
    return true;
}

void library_free(Library *lib)
{
    free(lib->games);
    memset(lib, 0, sizeof(*lib));
}
