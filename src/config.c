/*
 * PSXS5 - settings stored as key=value lines in <root>/psxs5.ini.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->aspect = ASPECT_AUTO;
    s->smooth = false; /* sharp pixels; "Smooth final scaling" is opt-in */
    s->internal_res = 1;
    s->upscale = 2;
    s->upscale_filter = UPSCALE_XBR;
    s->region = REGION_AUTO;
    s->dithering = true;
    s->analog = true; /* DualShock: it starts in digital mode, so digital-only games still work */
    s->boot_intro = true; /* the BIOS runs its full start-up (no fast boot) */
    s->cover_style = COVER_FLAT;
    s->cover_download = true;
    s->ui_sound = 0;  /* Soft */
    s->ui_volume = 1; /* 50 % */
    s->vibration = 4;
    s->quick_resume = true;
    s->update_check = true;
    s->pgxp = true; /* Beetle: no wobbling polygons */
    s->ra_popups = true;
    s->hd_textures = true;
    s->ra_tracker = true;
    s->trigger_effects = true;
    s->brightness = 1;
    s->game_fixes = true;
    s->disc_animation = true;
    s->negcon = true;
    s->touch_mouse = true;
    s->racing_triggers = true;
    for (int i = 0; i < 16; ++i)
        s->button_map[i] = (int8_t)i;
    for (int i = 0; i < SS_OPT_COUNT; ++i)
        s->ss_opt[i] = SS_OPTS[i].def;
}

static bool as_bool(const char *v)
{
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0;
}

/* Applies the keys in a file over what *s already holds. */
static bool config_apply(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    bool seen_vibration = false;
    int old_rumble = -1, old_strength = -1;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        char *key = line, *value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        if (strncmp(key, "swanstation_", 12) == 0)
        {
            /* a SwanStation core option, by name and value (so reordering the table keeps saved files valid) */
            for (int i = 0; i < SS_OPT_COUNT; ++i)
                if (strcmp(key, SS_OPTS[i].key) == 0)
                    for (int v = 0; v < SS_OPTS[i].count; ++v)
                        if (strcmp(value, SS_OPTS[i].values[v]) == 0)
                            s->ss_opt[i] = v;
        }
        else if (strcmp(key, "aspect") == 0)
            s->aspect = atoi(value) % ASPECT_COUNT;
        else if (strcmp(key, "smooth") == 0)
            s->smooth = as_bool(value);
        else if (strcmp(key, "show_fps") == 0)
            s->show_fps = as_bool(value);
        else if (strcmp(key, "region") == 0)
            s->region = atoi(value) % REGION_COUNT;
        else if (strcmp(key, "force_hle") == 0)
            s->force_hle = false; /* SwanStation has no built-in HLE BIOS: an old file may still say 1 */
        else if (strcmp(key, "dithering") == 0)
            s->dithering = as_bool(value);
        else if (strcmp(key, "cd_fast") == 0)
            s->cd_fast = as_bool(value);
        else if (strcmp(key, "analog") == 0)
            s->analog = as_bool(value);
        else if (strcmp(key, "state_slot") == 0)
            s->state_slot = atoi(value) % 10;
        else if (strcmp(key, "last_game") == 0)
            s->last_game = atoi(value);
        else if (strcmp(key, "stick_dpad") == 0)
            s->stick_dpad = atoi(value) % STICK_DPAD_COUNT;
        else if (strcmp(key, "integer_scale") == 0)
            s->integer_scale = as_bool(value);
        else if (strcmp(key, "internal_res") == 0)
        {
            int r = atoi(value);
            s->internal_res = r >= 1 && r <= 5 ? r : 1;
        }
        else if (strcmp(key, "upscale") == 0)
            s->upscale = atoi(value) >= 1 && atoi(value) <= 4 ? atoi(value) : 1;
        else if (strcmp(key, "upscale_filter") == 0)
            s->upscale_filter = atoi(value) % UPSCALE_FILTER_COUNT;
        else if (strcmp(key, "cover_style") == 0)
            s->cover_style = atoi(value) % COVER_STYLE_COUNT;
        else if (strcmp(key, "cover_download") == 0)
            s->cover_download = as_bool(value);
        else if (strcmp(key, "ui_sounds") == 0 && !as_bool(value))
            s->ui_sound = 5; /* older config: sounds off -> SFX_STYLE_OFF */
        else if (strcmp(key, "ui_sound") == 0)
            s->ui_sound = atoi(value) % 6;
        else if (strcmp(key, "language") == 0)
            s->language = atoi(value) % 5;
        else if (strcmp(key, "ui_volume") == 0)
            s->ui_volume = atoi(value) % 4;
        else if (strcmp(key, "vibration") == 0)
        {
            s->vibration = atoi(value) % 5;
            seen_vibration = true;
        }
        else if (strcmp(key, "rumble") == 0) /* older files: an on/off and a strength */
            old_rumble = as_bool(value) ? 1 : 0;
        else if (strcmp(key, "rumble_strength") == 0)
            old_strength = atoi(value) % 4;
        else if (strcmp(key, "sort_mode") == 0)
            s->sort_mode = atoi(value) % 8;
        else if (strcmp(key, "widescreen") == 0)
            s->widescreen = as_bool(value);
        else if (strcmp(key, "multitap") == 0)
            s->multitap = as_bool(value);
        else if (strcmp(key, "emulator") == 0)
            s->emulator = atoi(value) % EMU_COUNT;
        else if (strcmp(key, "renderer") == 0)
            s->renderer = atoi(value) % 2;
        else if (strcmp(key, "pgxp") == 0)
            s->pgxp = as_bool(value);
        else if (strcmp(key, "theme") == 0)
            s->theme = atoi(value) % 5;
        else if (strcmp(key, "hd_textures") == 0)
            s->hd_textures = as_bool(value);
        else if (strcmp(key, "shader") == 0)
            s->shader = atoi(value) % 5;
        else if (strcmp(key, "crop_edges") == 0)
            s->crop_edges = atoi(value) % 3;
        else if (strcmp(key, "ra_popups") == 0)
            s->ra_popups = as_bool(value);
        else if (strcmp(key, "ra_tracker") == 0)
            s->ra_tracker = as_bool(value);
        else if (strcmp(key, "ra_popup_style") == 0)
            s->ra_popup_style = atoi(value) % 3;
        else if (strcmp(key, "lightbar") == 0)
            s->lightbar = atoi(value) % 3;
        else if (strcmp(key, "stick_deadzone") == 0)
            s->stick_deadzone = atoi(value) % 5;
        else if (strcmp(key, "stick_response") == 0)
            s->stick_response = atoi(value) % 3;
        else if (strcmp(key, "trigger_effects") == 0)
            s->trigger_effects = as_bool(value);
        else if (strcmp(key, "racing_triggers") == 0)
            s->racing_triggers = as_bool(value);
        else if (strcmp(key, "rumble_feel") == 0)
            s->rumble_feel = atoi(value) % 4;
        else if (strcmp(key, "lightgun") == 0)
            s->lightgun = atoi(value) % 3;
        else if (strcmp(key, "overclock") == 0)
            s->overclock = atoi(value) % 3;
        else if (strcmp(key, "brightness") == 0)
            s->brightness = atoi(value) % 4;
        else if (strcmp(key, "colour") == 0)
            s->colour = atoi(value) % 6;
        else if (strcmp(key, "autosave") == 0)
            s->autosave = atoi(value) % 4;
        else if (strcmp(key, "bezel") == 0)
            s->bezel = as_bool(value);
        else if (strcmp(key, "msaa") == 0)
            s->msaa = atoi(value) % 5;
        else if (strcmp(key, "texture_filter") == 0)
            s->texture_filter = atoi(value) % 6;
        else if (strcmp(key, "filter_2d") == 0)
            s->filter_2d = as_bool(value);
        else if (strcmp(key, "supersampling") == 0)
            s->supersampling = as_bool(value);
        else if (strcmp(key, "deinterlace") == 0)
            s->deinterlace = atoi(value) % 3;
        else if (strcmp(key, "pal60") == 0)
            s->pal60 = as_bool(value);
        else if (strcmp(key, "game_fixes") == 0)
            s->game_fixes = as_bool(value);
        else if (strcmp(key, "fmv_smooth") == 0)
            s->fmv_smooth = as_bool(value);
        else if (strcmp(key, "true_colour") == 0)
            s->true_colour = as_bool(value);
        else if (strcmp(key, "boot_intro") == 0)
            s->boot_intro = as_bool(value);
        else if (strcmp(key, "sharpen") == 0)
            s->sharpen = atoi(value) % 3;
        else if (strcmp(key, "disc_animation") == 0)
            s->disc_animation = as_bool(value);
        else if (strcmp(key, "negcon") == 0)
            s->negcon = as_bool(value);
        else if (strcmp(key, "touch_mouse") == 0)
            s->touch_mouse = as_bool(value);
        else if (strcmp(key, "run_ahead") == 0)
            s->run_ahead = atoi(value) % 3;
        else if (strcmp(key, "fast_effects") == 0)
            s->fast_effects = as_bool(value);
        else if (strcmp(key, "rewind") == 0)
            s->rewind = as_bool(value);
        else if (strcmp(key, "quick_resume") == 0)
            s->quick_resume = as_bool(value);
        else if (strcmp(key, "crt") == 0)
            s->crt = atoi(value) % 3;
        else if (strcmp(key, "border") == 0)
            s->border = atoi(value) % 3;
        else if (strcmp(key, "remote") == 0)
            s->remote = as_bool(value);
        else if (strcmp(key, "update_check") == 0)
            s->update_check = as_bool(value);
        else if (strcmp(key, "background") == 0)
            s->background = atoi(value) % 2;
        else if (strcmp(key, "shelf_category") == 0)
            s->shelf_category = atoi(value) % 16;
        else if (strcmp(key, "button_map") == 0)
        {
            char *p = value;
            for (int i = 0; i < 16 && *p; ++i)
            {
                int v = (int)strtol(p, &p, 10);
                s->button_map[i] = (int8_t)(v >= -1 && v < 16 ? v : i);
                if (*p == ',')
                    ++p;
            }
        }
    }
    fclose(f);
    if (!seen_vibration && (old_rumble >= 0 || old_strength >= 0))
    {
        bool on = old_rumble >= 0 ? old_rumble == 1 : s->vibration > 0;
        int strength = old_strength >= 0 ? old_strength : s->vibration > 0 ? s->vibration - 1 : 3;
        s->vibration = on ? strength + 1 : 0;
    }
    return true;
}

void config_load(Settings *s, const char *path)
{
    config_defaults(s);
    config_apply(s, path);
}

bool config_load_game(Settings *out, const Settings *global, const char *path)
{
    *out = *global;
    if (!config_apply(out, path))
        return false;
    /* these always follow the console-wide settings */
    out->last_game = global->last_game;
    out->cover_style = global->cover_style;
    out->cover_download = global->cover_download;
    out->ui_sound = global->ui_sound;
    out->ui_volume = global->ui_volume;
    out->language = global->language;
    out->sort_mode = global->sort_mode;
    out->shelf_category = global->shelf_category;
    out->background = global->background;
    out->remote = global->remote;
    out->update_check = global->update_check;
    out->quick_resume = global->quick_resume;
    return true;
}

bool config_save(const Settings *s, const char *path)
{
    char temp[PSXS5_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    if (!f)
        return false;
    fprintf(f,
            "# PSXS5 settings\n"
            "aspect=%d\nsmooth=%d\nshow_fps=%d\nregion=%d\nforce_hle=%d\n"
            "dithering=%d\ncd_fast=%d\nanalog=%d\nstate_slot=%d\nlast_game=%d\n"
            "cover_style=%d\ncover_download=%d\nui_sound=%d\nui_volume=%d\n"
            "integer_scale=%d\ninternal_res=%d\nupscale=%d\nupscale_filter=%d\nstick_dpad=%d\nlanguage=%d\n"
            "vibration=%d\nsort_mode=%d\nshelf_category=%d\nbackground=%d\n",
            s->aspect, s->smooth, s->show_fps, s->region, s->force_hle, s->dithering,
            s->cd_fast, s->analog, s->state_slot, s->last_game, s->cover_style,
            s->cover_download, s->ui_sound, s->ui_volume, s->integer_scale, s->internal_res, s->upscale,
            s->upscale_filter, s->stick_dpad, s->language, s->vibration,
            s->sort_mode, s->shelf_category, s->background);
    fprintf(f, "widescreen=%d\nmultitap=%d\nrewind=%d\nquick_resume=%d\ncrt=%d\nborder=%d\nremote=%d\nupdate_check=%d\n",
            s->widescreen, s->multitap, s->rewind, s->quick_resume, s->crt, s->border, s->remote,
            s->update_check);
    fprintf(f, "renderer=%d\n", s->renderer);
    fprintf(f, "emulator=%d\npgxp=%d\nra_popups=%d\nra_tracker=%d\ncrop_edges=%d\n", s->emulator, s->pgxp,
            s->ra_popups, s->ra_tracker, s->crop_edges);
    fprintf(f, "ra_popup_style=%d\nlightbar=%d\nshader=%d\nhd_textures=%d\ntheme=%d\n", s->ra_popup_style,
            s->lightbar, s->shader, s->hd_textures, s->theme);
    fprintf(f, "stick_deadzone=%d\nstick_response=%d\ntrigger_effects=%d\nracing_triggers=%d\nrumble_feel=%d\n",
            s->stick_deadzone, s->stick_response, s->trigger_effects, s->racing_triggers, s->rumble_feel);
    fprintf(f, "lightgun=%d\noverclock=%d\nbrightness=%d\ncolour=%d\nautosave=%d\nbezel=%d\n", s->lightgun,
            s->overclock, s->brightness, s->colour, s->autosave, s->bezel);
    fprintf(f, "msaa=%d\ntexture_filter=%d\nfilter_2d=%d\nsupersampling=%d\ndeinterlace=%d\npal60=%d\ngame_fixes=%d\n",
            s->msaa, s->texture_filter, s->filter_2d, s->supersampling, s->deinterlace, s->pal60, s->game_fixes);
    fprintf(f, "fmv_smooth=%d\ntrue_colour=%d\nboot_intro=%d\nsharpen=%d\ndisc_animation=%d\nnegcon=%d\n",
            s->fmv_smooth, s->true_colour, s->boot_intro, s->sharpen, s->disc_animation, s->negcon);
    fprintf(f, "touch_mouse=%d\nrun_ahead=%d\nfast_effects=%d\n", s->touch_mouse, s->run_ahead, s->fast_effects);
    for (int i = 0; i < SS_OPT_COUNT; ++i)
    {
        int v = s->ss_opt[i];
        if (v >= 0 && v < SS_OPTS[i].count && v != SS_OPTS[i].def) /* only what differs from the default */
            fprintf(f, "%s=%s\n", SS_OPTS[i].key, SS_OPTS[i].values[v]);
    }
    fprintf(f, "button_map=");
    for (int i = 0; i < 16; ++i)
        fprintf(f, i ? ",%d" : "%d", s->button_map[i]);
    fprintf(f, "\n");
    bool ok = fclose(f) == 0;
    return ok && rename(temp, path) == 0;
}

void config_paths(Paths *p, const char *root)
{
    str_copy(p->root, sizeof(p->root), root);
    path_join(p->games, sizeof(p->games), root, "games");
    path_join(p->bios, sizeof(p->bios), root, "bios");
    path_join(p->saves, sizeof(p->saves), root, "saves");
    path_join(p->states, sizeof(p->states), root, "states");
    path_join(p->cheats, sizeof(p->cheats), root, "cheats");
    path_join(p->covers, sizeof(p->covers), root, "covers");
    path_join(p->logs, sizeof(p->logs), root, "logs");
    path_join(p->config, sizeof(p->config), root, "psxs5.ini");
    str_copy(p->user, sizeof(p->user), root);
}

void config_user_paths(Paths *p, const char *dir)
{
    str_copy(p->user, sizeof(p->user), dir);
    path_join(p->saves, sizeof(p->saves), dir, "saves");
    path_join(p->states, sizeof(p->states), dir, "states");
    path_join(p->config, sizeof(p->config), dir, "psxs5.ini");
}
