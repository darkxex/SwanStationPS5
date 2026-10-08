/*
 * PSXS5 - tips for a game, shown in the shelf's Details panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Some come from what PSXS5 knows about the game here (which emulator runs
 * it, its discs, a manual), some from assets/game-tips.txt (light guns,
 * multitap games, LibCrypt discs...).
 */
#include "tips.h"

#include "cheats.h"
#include "core/host.h"
#include "i18n.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *known;      /* assets/game-tips.txt, read once */
static bool known_tried;

static const char *region_code(const char *serial)
{
    if (!strncmp(serial, "SLUS", 4) || !strncmp(serial, "SCUS", 4) || !strncmp(serial, "PAPX", 4))
        return "US";
    if (!strncmp(serial, "SLES", 4) || !strncmp(serial, "SCES", 4) || !strncmp(serial, "SCED", 4))
        return "EU";
    return serial[0] ? "JP" : "";
}

static bool has_manual(const Game *g)
{
    static const char *const first[] = {"manual/1.jpg", "manual/1.png", "manual/01.jpg", "manual/01.png",
                                        "manual/001.jpg", "manual/001.png"};
    for (size_t i = 0; i < sizeof(first) / sizeof(first[0]); ++i)
    {
        char path[PSXS5_PATH_MAX];
        path_join(path, sizeof(path), g->folder, first[i]);
        FILE *f = fopen(path, "rb");
        if (f)
        {
            fclose(f);
            return true;
        }
    }
    return false;
}

int tips_for(const Game *g, const Settings *settings, char lines[][TIP_LEN], int max)
{
    int n = 0;
    /* which emulator, and why not Beetle */
    const char *why = NULL;
    const char *emu = host_emulator_for(settings, g->serial, &why);
    if (n < max)
    {
        if (why)
            snprintf(lines[n++], TIP_LEN, tr("Runs on %s: %s."), emu, tr(why));
        else
            snprintf(lines[n++], TIP_LEN, tr("Runs on %s."), emu);
    }
    if (g->discs > 1 && n < max)
        snprintf(lines[n++], TIP_LEN, tr("%d discs: hold the touchpad and press R1 to swap."), g->discs);
    if (g->folder[0] && has_manual(g) && n < max)
        str_copy(lines[n++], TIP_LEN, tr("Has a manual: in the game, SwanStationPS5 menu > Manual."));

    if (!known && !known_tried)
    {
        known_tried = true;
        char path[PSXS5_PATH_MAX];
        plat_asset_path(path, sizeof(path), "game-tips.txt");
        FILE *f = fopen(path, "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            known = size > 0 ? malloc((size_t)size + 1) : NULL;
            if (known)
                known[fread(known, 1, (size_t)size, f)] = '\0';
            fclose(f);
        }
    }
    const char *region = region_code(g->serial);
    for (const char *line = known; line && *line && n < max; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
    {
        if (*line == '#' || *line == '\n' || *line == '\r')
            continue;
        char buf[400];
        size_t len = strcspn(line, "\r\n");
        if (len >= sizeof(buf))
            continue;
        memcpy(buf, line, len);
        buf[len] = '\0';
        char *title = buf, *where = strchr(title, '\t'), *tip = where ? strchr(where + 1, '\t') : NULL;
        if (!tip)
            continue;
        *where++ = '\0';
        *tip++ = '\0';
        if (strcmp(where, "any") != 0 && strcmp(where, region) != 0)
            continue;
        if (titles_match(title, g->title) || titles_match(title, g->disc_name))
            str_copy(lines[n++], TIP_LEN, tr(tip));
    }
    return n;
}
