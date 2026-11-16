/*
 * SwanStationPS5 - what DuckStation's game database knows about each disc: genre,
 * release, players, the controllers it takes, and the settings it's known to
 * break with.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The database (data/resources/gamedb.yaml in github.com/stenzek/duckstation)
 * isn't part of SwanStationPS5: the console downloads it once and keeps a compact
 * index, <root>/cache/gamedb.txt (one line per serial, sorted), refreshed
 * after a month. Until it's there, every lookup simply finds nothing.
 */
#include "gamedb.h"

#include "app.h"
#include "disc.h"
#include "net.h"

#include <SDL2/SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define GAMEDB_URL "https://raw.githubusercontent.com/stenzek/duckstation/master/data/resources/gamedb.yaml"
#define REFRESH_DAYS 30

static SDL_atomic_t state; /* 0 idle, 1 downloading, 2 a new index is there */
/* the index's first line: the version of its format */
#define GAMEDB_HEADER "#v13"

static char yaml_path[SwanStationPS5_PATH_MAX], index_path[SwanStationPS5_PATH_MAX];

static GameInfo *games;
static int game_count;
static bool loaded;

/* ---------------------------------------------------------------- building the index */

static const struct
{
    const char *name;
    unsigned flag;
} WORDS[] = {
    {"GunCon", GDB_GUNCON},
    {"Justifier", GDB_JUSTIFIER},
    {"NeGcon", GDB_NEGCON},
    {"NeGconRumble", GDB_NEGCON},
    {"AnalogController", GDB_ANALOG},
    {"PlayStationMouse", GDB_MOUSE},
    {"DisableWidescreen", GDB_NO_WIDESCREEN},
    {"DisablePGXP", GDB_NO_PGXP},
    {"ForcePGXPCPUMode", GDB_PGXP_CPU},
    {"DisableUpscaling", GDB_NO_UPSCALING},
    {"DisableTextureFiltering", GDB_NO_TEXTURE_FILTER},
    {"DisableSpriteTextureFiltering", GDB_NO_SPRITE_FILTER},
    {"ForceDeinterlacing", GDB_DEINTERLACE},
    {"DisableMultitap", GDB_NO_MULTITAP},
    {"DisableAutoAnalogMode", GDB_NO_AUTO_ANALOG},
    {"DisableCDROMReadSpeedup", GDB_NO_CD_SPEEDUP},
    {"DisableCDROMSpeedupOnMDEC", GDB_NO_CD_SPEEDUP},
    {"ForceRecompilerICache", GDB_ICACHE},
    {"ForcePGXPVertexCache", GDB_PGXP_VCACHE},
    {"DisablePGXPCulling", GDB_NO_PGXP_CULL},
    {"DisableTrueColor", GDB_NO_TRUE_COLOR},
    {"ForceSoftwareRendererForReadbacks", GDB_SW_READBACKS},
    {"ForceRoundTextureCoordinates", GDB_ROUND_TEXCOORDS},
    {"DisablePGXPOn2DPolygons", GDB_NO_PGXP_2D},
    {"DisablePGXPColorCorrection", GDB_NO_PGXP_COLOR},
    {"DisableCDROMSeekSpeedup", GDB_NO_SEEK_SPEEDUP},
    {"DisableAllBordersCrop", GDB_NO_BORDERS_CROP},
    {"ForceFullTrueColor", GDB_FULL_TRUE_COLOR},
    {"NormalizePGXPDepth", GDB_NORM_DEPTH},
};

/* the database's own settings for a disc that are applied, and the short name each has in the index */
static const struct
{
    const char *name, *tag;
} SETTINGS[] = {
    {"displayActiveStartOffset", "as"}, {"displayActiveEndOffset", "ae"}, {"displayLineStartOffset", "ls"},
    {"dmaMaxSliceTicks", "ds"},         {"dmaHaltTicks", "dh"},           {"gpuPGXPTolerance", "pt"},
    {"gpuPGXPPreserveProjFP", "pp"},    {"gpuFilterFramebufferUploads", "fu"},
    {"displayCropMode", "cm"},
    {"gpuLineDetectMode", "ld"},
    {"gpuPGXPDepthThreshold", "pd"},
    {"gpuPGXPNormalizeDepth", "nd"},
};

typedef struct
{
    char serial[24];
    char codes[8][24];
    int code_count;
    char name[128], genre[64], developer[64], publisher[64];
    int year, min_players, max_players;
    unsigned flags;
    char settings[128]; /* "as=-8;ds=100;...", from SETTINGS */
} Record;

static void unquote(char *out, size_t size, const char *v)
{
    while (*v == ' ')
        ++v;
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\r'))
        --n;
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"')
    {
        ++v;
        n -= 2;
    }
    if (n >= size)
        n = size - 1;
    size_t w = 0;
    for (size_t i = 0; i < n; ++i)
        if (v[i] != '\t' && v[i] != '\n')
            out[w++] = v[i];
    out[w] = '\0';
}

static void write_record(FILE *out, const Record *r)
{
    if (!r->serial[0])
        return;
    /* the record under its own serial and under each other code of the disc */
    for (int k = -1; k < r->code_count; ++k)
    {
        char serial[16];
        if (!disc_format_serial(k < 0 ? r->serial : r->codes[k], serial, sizeof(serial)))
            continue;
        fprintf(out, "%s\t%s\t%s\t%d\t%s\t%s\t%d\t%d\t%x\t%s\n", serial, r->name, r->genre, r->year,
                r->developer, r->publisher, r->min_players, r->max_players, r->flags, r->settings);
    }
}

static int cmp_lines(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* gamedb.yaml -> gamedb.txt, line by line (the YAML's own simple layout). */
static bool build_index(const char *yaml, const char *index)
{
    FILE *in = fopen(yaml, "r");
    if (!in)
        return false;
    char temp[SwanStationPS5_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", index);
    FILE *out = fopen(temp, "w");
    if (!out)
    {
        fclose(in);
        return false;
    }
    Record r;
    memset(&r, 0, sizeof(r));
    char section[32] = "", line[512];
    while (fgets(line, sizeof(line), in))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *hash = strstr(line, " #");
        if (hash)
            *hash = '\0';
        if (line[0] && line[0] != ' ' && line[0] != '#')
        {
            /* a new disc: "SLUS-00067:" */
            write_record(out, &r);
            memset(&r, 0, sizeof(r));
            char *colon = strchr(line, ':');
            if (colon)
                *colon = '\0';
            str_copy(r.serial, sizeof(r.serial), line);
            section[0] = '\0';
            continue;
        }
        if (!strncmp(line, "  ", 2) && line[2] != ' ')
        {
            /* "  name: ..." or a section: "  controllers:" */
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            str_copy(section, sizeof(section), line + 2);
            if (!strcmp(section, "name"))
                unquote(r.name, sizeof(r.name), colon + 1);
            continue;
        }
        if (!strncmp(line, "    - ", 6))
        {
            const char *item = line + 6;
            char bare[96]; /* "DisableWidescreen # Breaks the menus." without its comment */
            str_copy(bare, sizeof(bare), item);
            bare[strcspn(bare, " #")] = '\0';
            if (!strcmp(section, "codes") && r.code_count < 8)
                unquote(r.codes[r.code_count++], sizeof(r.codes[0]), item);
            else
                for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); ++i)
                    if (!strcmp(bare, WORDS[i].name))
                        r.flags |= WORDS[i].flag;
            continue;
        }
        if (!strncmp(line, "    ", 4) && !strcmp(section, "settings"))
        {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            char value[32];
            unquote(value, sizeof(value), colon + 1);
            value[strcspn(value, " #")] = '\0'; /* "256x224 # Enables background texture filtering." */
            if (!strcmp(value, "true"))
                str_copy(value, sizeof(value), "1");
            else if (!strcmp(value, "false"))
                str_copy(value, sizeof(value), "0");
            for (size_t i = 0; i < sizeof(SETTINGS) / sizeof(SETTINGS[0]); ++i)
                if (!strcmp(line + 4, SETTINGS[i].name) && value[0])
                {
                    size_t used = strlen(r.settings);
                    snprintf(r.settings + used, sizeof(r.settings) - used, "%s%s=%s", used ? ";" : "", SETTINGS[i].tag,
                             value);
                }
            continue;
        }
        if (!strncmp(line, "    ", 4) && !strcmp(section, "metadata"))
        {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            const char *key = line + 4, *v = colon + 1;
            if (!strcmp(key, "genre"))
                unquote(r.genre, sizeof(r.genre), v);
            else if (!strcmp(key, "developer"))
                unquote(r.developer, sizeof(r.developer), v);
            else if (!strcmp(key, "publisher"))
                unquote(r.publisher, sizeof(r.publisher), v);
            else if (!strcmp(key, "releaseDate"))
            {
                char date[32];
                unquote(date, sizeof(date), v);
                r.year = atoi(date);
            }
            else if (!strcmp(key, "minPlayers"))
                r.min_players = atoi(v);
            else if (!strcmp(key, "maxPlayers"))
                r.max_players = atoi(v);
            else if (!strcmp(key, "multitap") && strstr(v, "true"))
                r.flags |= GDB_MULTITAP;
            else if (!strcmp(key, "vibration") && strstr(v, "true"))
                r.flags |= GDB_VIBRATION;
        }
    }
    write_record(out, &r);
    fclose(in);
    bool ok = fclose(out) == 0;

    /* sorted, for the lookups */
    FILE *f = ok ? fopen(temp, "rb") : NULL;
    char *text = NULL;
    long size = 0;
    if (f)
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = size > 0 ? malloc((size_t)size + 1) : NULL;
        if (text)
            text[fread(text, 1, (size_t)size, f)] = '\0';
        fclose(f);
    }
    ok = false;
    if (text)
    {
        int n = 0;
        for (char *p = text; *p; ++p)
            n += *p == '\n';
        char **lines = malloc(sizeof(char *) * (size_t)(n + 1));
        if (lines)
        {
            int k = 0;
            for (char *p = text; *p && k <= n;)
            {
                lines[k++] = p;
                char *nl = strchr(p, '\n');
                if (!nl)
                    break;
                *nl = '\0';
                p = nl + 1;
            }
            qsort(lines, (size_t)k, sizeof(char *), cmp_lines);
            FILE *sorted = fopen(temp, "w");
            if (sorted)
            {
                fprintf(sorted, "%s\n", GAMEDB_HEADER);
                for (int i = 0; i < k; ++i)
                    if (lines[i][0])
                        fprintf(sorted, "%s\n", lines[i]);
                ok = fclose(sorted) == 0;
            }
            free(lines);
        }
        free(text);
    }
    if (ok)
        ok = rename(temp, index) == 0;
    else
        remove(temp);
    return ok;
}

static int fetch_main(void *unused)
{
    (void)unused;
    char part[SwanStationPS5_PATH_MAX + 8];
    snprintf(part, sizeof(part), "%s.part", yaml_path);
    NetResult r = net_download(GAMEDB_URL, part);
    bool ok = r == NET_OK && build_index(part, index_path);
    remove(part);
    SwanStationPS5_log("gamedb: %s", ok ? "index written" : r == NET_OK ? "could not read the database" : "download failed");
    SDL_AtomicSet(&state, ok ? 2 : 0);
    return 0;
}

/* ---------------------------------------------------------------- lookups */

static bool index_current(FILE *f)
{
    char line[16];
    return fgets(line, sizeof(line), f) && !strncmp(line, GAMEDB_HEADER "\n", sizeof(GAMEDB_HEADER));
}

static void load(void)
{
    loaded = true;
    free(games);
    games = NULL;
    game_count = 0;
    FILE *f = fopen(index_path, "r");
    if (!f)
        return;
    if (!index_current(f))
    {
        fclose(f);
        return;
    }
    int cap = 0;
    char line[600];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *field[10] = {0};
        char *p = line;
        for (int i = 0; i < 10 && p; ++i)
        {
            field[i] = p;
            p = strchr(p, '\t');
            if (p)
                *p++ = '\0';
        }
        if (!field[8])
            continue;
        if (game_count == cap)
        {
            int more = cap ? cap * 2 : 4096;
            GameInfo *g = realloc(games, sizeof(GameInfo) * (size_t)more);
            if (!g)
                break;
            games = g;
            cap = more;
        }
        GameInfo *g = &games[game_count++];
        memset(g, 0, sizeof(*g));
        str_copy(g->serial, sizeof(g->serial), field[0]);
        str_copy(g->name, sizeof(g->name), field[1]);
        str_copy(g->genre, sizeof(g->genre), field[2]);
        g->year = atoi(field[3]);
        str_copy(g->developer, sizeof(g->developer), field[4]);
        str_copy(g->publisher, sizeof(g->publisher), field[5]);
        g->min_players = atoi(field[6]);
        g->max_players = atoi(field[7]);
        g->flags = (unsigned)strtoul(field[8], NULL, 16);
        for (char *tag = field[9] ? field[9] : (char *)""; *tag;)
        {
            char *end = strchr(tag, ';');
            if (end)
                *end = '\0';
            const char *eq = strchr(tag, '=');
            if (eq && eq - tag == 2)
            {
                int n = atoi(eq + 1);
                if (!strncmp(tag, "as", 2))
                    g->active_start = n, g->set |= GDT_ACTIVE_START;
                else if (!strncmp(tag, "ae", 2))
                    g->active_end = n, g->set |= GDT_ACTIVE_END;
                else if (!strncmp(tag, "ls", 2))
                    g->line_start = n, g->set |= GDT_LINE_START;
                else if (!strncmp(tag, "ds", 2))
                    g->dma_slice = n, g->set |= GDT_DMA_SLICE;
                else if (!strncmp(tag, "dh", 2))
                    g->dma_halt = n, g->set |= GDT_DMA_HALT;
                else if (!strncmp(tag, "pt", 2))
                    g->pgxp_tolerance = (float)atof(eq + 1), g->set |= GDT_PGXP_TOLERANCE;
                else if (!strncmp(tag, "pd", 2))
                    g->pgxp_depth_threshold = (float)atof(eq + 1), g->set |= GDT_DEPTH_THRESHOLD;
                else if (!strncmp(tag, "nd", 2))
                {
                    g->norm_depth = !strcmp(eq + 1, "Always") ? 1 : !strcmp(eq + 1, "DepthBufferOnly") ? 2 : 0;
                    if (g->norm_depth)
                        g->set |= GDT_NORM_DEPTH;
                }
                else if (!strncmp(tag, "cm", 2))
                {
                    const char *v = eq + 1;
                    g->crop_mode = !strcmp(v, "Borders") ? 2 : !strcmp(v, "Overscan") ? 1 : 0;
                    g->set |= GDT_CROP_MODE;
                }
                else if (!strncmp(tag, "ld", 2))
                {
                    const char *v = eq + 1;
                    g->line_mode = !strcmp(v, "AggressiveTriangles") ? 3 : !strcmp(v, "BasicTriangles") ? 2 : !strcmp(v, "Quads") ? 1 : 0;
                    if (g->line_mode)
                        g->set |= GDT_LINE_MODE;
                }
                else if (!strncmp(tag, "pp", 2))
                    g->pgxp_proj = n != 0, g->set |= GDT_PGXP_PROJ;
                else if (!strncmp(tag, "fu", 2) &&
                         sscanf(eq + 1, "%dx%d", &g->filter_uploads_w, &g->filter_uploads_h) == 2)
                    g->set |= GDT_FILTER_UPLOADS;
            }
            if (!end)
                break;
            tag = end + 1;
        }
    }
    fclose(f);
    SwanStationPS5_log("gamedb: %d discs known", game_count);
}

/* the indexes that carried the format in their name (gamedb-3.txt...) */
static void remove_old_indexes(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, "gamedb-", 7) || n < 11 || strcmp(e->d_name + n - 4, ".txt"))
            continue;
        char path[SwanStationPS5_PATH_MAX];
        path_join(path, sizeof(path), dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

void gamedb_start(void)
{
    path_join(index_path, sizeof(index_path), app.paths.root, "cache/" GAMEDB_INDEX_FILE);
    path_join(yaml_path, sizeof(yaml_path), app.paths.root, "cache/gamedb.yaml");
    char dir[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "cache");
    make_dirs(dir);
    remove_old_indexes(dir);
    struct stat st;
    bool have = stat(index_path, &st) == 0;
    if (have)
    {
        FILE *f = fopen(index_path, "r");
        have = f && index_current(f);
        if (f)
            fclose(f);
    }
    bool old = have && time(NULL) - st.st_mtime > REFRESH_DAYS * 86400L;
    if ((!have || old) && net_available() && SDL_AtomicCAS(&state, 0, 1))
    {
        SDL_Thread *t = SDL_CreateThread(fetch_main, "gamedb", NULL);
        if (t)
            SDL_DetachThread(t);
        else
            SDL_AtomicSet(&state, 0);
    }
}

const GameInfo *gamedb_get(const char *serial)
{
    if (SDL_AtomicCAS(&state, 2, 0))
        loaded = false; /* a fresh index arrived */
    if (!loaded)
        load();
    char want[16];
    if (!serial || !serial[0] || !games || !disc_format_serial(serial, want, sizeof(want)))
        return NULL;
    int lo = 0, hi = game_count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2, c = strcmp(games[mid].serial, want);
        if (!c)
            return &games[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}
