/*
 * SwanStationPS5 - settings stored as key=value lines in <root>/SwanStationPS5.ini.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"

#include "i18n.h"
#include "platform/platform.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->language = plat_system_language(); /* until the ini says otherwise: the console's */
    s->smooth = false; /* sharp pixels; "Smooth final scaling" is opt-in */
    s->internal_res = 9; /* 9x (4K) */
    s->cd_speed = 1;
    s->nfs_timeout = 2;
    s->frame_pacing = true; /* Video sync */
    s->true_colour = true; /* True Color is on by default */
    s->upscale = 2;
    s->upscale_filter = UPSCALE_XBR;
    s->region = REGION_AUTO;
    s->dithering = true;
    s->lightgun = 1; /* Off */
    s->analog = true; /* DualShock: it starts in digital mode, so digital-only games still work */
    s->boot_intro = true; /* the BIOS runs its full start-up (no fast boot) */
    s->cover_style = COVER_FLAT;
    s->cover_download = true;
    s->ui_sound = 0;  /* Soft */
    s->ui_volume = 1; /* 50 % */
    s->vibration = 4;
    s->menu_combo = 1;
    s->menu_hold = 1;
    s->quick_resume = true;
    s->update_check = true;
    snprintf(s->usb_folder, sizeof(s->usb_folder), "psx"); /* /mnt/ext0/psx, /mnt/ext1/psx and /mnt/usb0/psx to /mnt/usb7/psx */
    s->bezel = false; /* the game's artwork frame, when there is one */
    s->theme = THEME_GRAYRAT; /* Grayrat (ui/theme.h) */
    s->pgxp = true; /* no wobbling polygons */
    s->ra_popups = true;
    s->hd_textures = true;
    s->ra_tracker = true;
    s->trigger_effects = 1;
    s->lightbar = 3; /* red in analog mode, off in digital */
    s->brightness = 1;
    s->game_fixes = true;
    s->disc_animation = true;
    s->jewel_cases = true;
    s->case_lighting = true;
    s->disc_ask = true;
    s->install_to_ext = false;
    s->ntfs_enabled = true;
    s->negcon = false;
    s->touch_mouse = false;
    s->racing_triggers = false;
    for (int i = 0; i < 16; ++i)
        s->button_map[i] = (int8_t)i;
    s->aspect_before_wide = -1;
    s->mc_w = s->mc_h = 100;
    for (int i = 0; i < SS_OPT_COUNT; ++i)
        s->ss_opt[i] = SS_OPTS[i].def;
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
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
    char line[1024]; /* long enough for nfs_shares */
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
        if (strcmp(key, "aspect_before_widescreen") == 0)
        {
            /* the aspect ratio the widescreen hack replaced with 16:9, by value */
            for (int i = 0; i < SS_OPT_COUNT; ++i)
                if (strcmp(SS_OPTS[i].key, "swanstation_Display_AspectRatio") == 0)
                    for (int v = 0; v < SS_OPTS[i].count; ++v)
                        if (strcmp(value, SS_OPTS[i].values[v]) == 0)
                            s->aspect_before_wide = v;
        }
        else if (strncmp(key, "swanstation_", 12) == 0)
        {
            /* a SwanStation core option, by name and value (so reordering the table keeps saved files valid) */
            for (int i = 0; i < SS_OPT_COUNT; ++i)
                if (strcmp(key, SS_OPTS[i].key) == 0)
                {
                    int v = ss_opt_find(i, value, true); /* a BIOS file not seen in the folder is kept by its name */
                    if (v >= 0)
                        s->ss_opt[i] = v;
                }
        }
        else if (strcmp(key, "smooth") == 0)
            s->smooth = as_bool(value);
        else if (strcmp(key, "show_fps") == 0)
            s->show_fps = as_bool(value);
        else if (strcmp(key, "region") == 0)
            s->region = atoi(value) % REGION_COUNT;
        else if (strcmp(key, "dithering") == 0)
            s->dithering = as_bool(value);
        else if (strcmp(key, "cd_speed") == 0)
            s->cd_speed = atoi(value) >= 1 && atoi(value) <= 10 ? atoi(value) : 1;
        else if (strcmp(key, "cd_fast") == 0) /* before 1.2.9: on was 4x */
            s->cd_speed = as_bool(value) ? 4 : 1;
        else if (strcmp(key, "analog") == 0)
            s->analog = as_bool(value);
        else if (strcmp(key, "state_slot") == 0)
            s->state_slot = atoi(value) % 10;
        else if (strcmp(key, "last_game") == 0)
            s->last_game = atoi(value);
        else if (strcmp(key, "stick_dpad") == 0)
            s->stick_dpad = atoi(value) % STICK_DPAD_COUNT;
        else if (strcmp(key, "menu_combo") == 0)
            s->menu_combo = atoi(value) >= 0 && atoi(value) < 3 ? atoi(value) : 1;
        else if (strcmp(key, "menu_hold") == 0)
            s->menu_hold = atoi(value) >= 0 && atoi(value) < 3 ? atoi(value) : 1;
        else if (strcmp(key, "integer_scale") == 0)
            s->integer_scale = as_bool(value);
        else if (strcmp(key, "internal_scale") == 0)
        {
            int r = atoi(value);
            s->internal_res = r >= 1 && r <= 16 ? r : 1;
        }
        else if (strcmp(key, "internal_res") == 0) /* before 1.2.9: 1..5 = 1x, 2x, 4x, 8x, 16x */
        {
            static const int scales[] = {1, 1, 2, 4, 8, 16};
            int r = atoi(value);
            s->internal_res = r >= 1 && r <= 5 ? scales[r] : 1;
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
        else if (strcmp(key, "renderer") == 0)
            s->renderer = atoi(value) % 2;
        else if (strcmp(key, "pgxp") == 0)
            s->pgxp = as_bool(value);
        else if (strcmp(key, "theme") == 0)
        {
            int theme = atoi(value); /* a number that is not a theme (an older or newer file) leaves the default */
            if (theme >= 0 && theme < THEME_COUNT)
                s->theme = theme;
        }
        else if (strcmp(key, "hd_textures") == 0)
            s->hd_textures = as_bool(value);
        else if (strcmp(key, "shader") == 0)
            s->shader = atoi(value) % 17 == 3 || atoi(value) % 17 == 4 || atoi(value) % 17 == 11 ? 0 : atoi(value) % 17; /* 3, 4 and 11 are gone */
        else if (strcmp(key, "crop_edges") == 0)
            s->crop_edges = atoi(value) % 3;
        else if (strcmp(key, "manual_crop") == 0)
            s->manual_crop = as_bool(value);
        else if (strcmp(key, "mc_w") == 0)
            s->mc_w = clamp_int(atoi(value), 25, 150);
        else if (strcmp(key, "mc_h") == 0)
            s->mc_h = clamp_int(atoi(value), 25, 150);
        else if (strcmp(key, "mc_x") == 0)
            s->mc_x = clamp_int(atoi(value), -500, 500);
        else if (strcmp(key, "mc_y") == 0)
            s->mc_y = clamp_int(atoi(value), -500, 500);
        else if (strcmp(key, "ra_popups") == 0)
            s->ra_popups = as_bool(value);
        else if (strcmp(key, "ra_tracker") == 0)
            s->ra_tracker = as_bool(value);
        else if (strcmp(key, "ra_popup_style") == 0)
            s->ra_popup_style = atoi(value) % 3;
        else if (strcmp(key, "lightbar") == 0)
            s->lightbar = atoi(value) % 4;
        else if (strcmp(key, "stick_deadzone") == 0)
            s->stick_deadzone = atoi(value) % 5;
        else if (strcmp(key, "stick_response") == 0)
            s->stick_response = atoi(value) % 3;
        else if (strcmp(key, "trigger_effects") == 0)
            {
            int v = atoi(value);
            s->trigger_effects = v >= 0 && v <= 5 ? v : 1;
        }
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
        else if (strcmp(key, "disc_in_tray") == 0)
            s->disc_in_tray = as_bool(value);
        else if (strcmp(key, "disc_animation") == 0)
            s->disc_animation = as_bool(value);
        else if (strcmp(key, "disc_ask") == 0)
            s->disc_ask = as_bool(value);
        else if (strcmp(key, "ntfs_enabled") == 0)
            s->ntfs_enabled = as_bool(value);
        else if (strcmp(key, "install_to_ext") == 0)
            s->install_to_ext = as_bool(value);
        else if (strcmp(key, "jewel_cases") == 0)
            s->jewel_cases = as_bool(value);
        else if (strcmp(key, "case_lighting") == 0)
            s->case_lighting = as_bool(value);
        else if (strcmp(key, "negcon") == 0)
            s->negcon = as_bool(value);
        else if (strcmp(key, "touch_mouse") == 0)
            s->touch_mouse = as_bool(value);
        else if (strcmp(key, "run_ahead") == 0)
            s->run_ahead = atoi(value) % 11;
        else if (strcmp(key, "fast_effects") == 0)
            s->fast_effects = as_bool(value);
        else if (strcmp(key, "nostalgia") == 0)
            s->nostalgia = as_bool(value);
        else if (strcmp(key, "frame_pacing") == 0)
            s->frame_pacing = as_bool(value);
        else if (strcmp(key, "video_out") == 0)
            s->video_out = atoi(value) % 2;
        else if (strcmp(key, "framegen") == 0)
            s->framegen = as_bool(value) ? 1 : atoi(value) == 2 ? 2 : 0;
        else if (strcmp(key, "usb_folder") == 0)
            snprintf(s->usb_folder, sizeof(s->usb_folder), "%s", value);
        else if (strcmp(key, "nfs_timeout") == 0)
            s->nfs_timeout = atoi(value) >= 0 && atoi(value) <= 4 ? atoi(value) : 2;
        else if (strcmp(key, "nfs_shares") == 0)
            snprintf(s->nfs_shares, sizeof(s->nfs_shares), "%s", value);
        else if (strcmp(key, "fsr") == 0)
            s->fsr = as_bool(value);
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
    /* the file only lists the core options that differ from their default, so the rest are the default
     * and not whatever the console-wide settings hold */
    out->aspect_before_wide = -1;
    for (int i = 0; i < SS_OPT_COUNT; ++i)
        out->ss_opt[i] = SS_OPTS[i].def;
    if (!config_apply(out, path))
    {
        memcpy(out->ss_opt, global->ss_opt, sizeof(out->ss_opt));
        out->aspect_before_wide = global->aspect_before_wide;
        return false;
    }
    /* these always follow the console-wide settings */
    for (int i = 0; i < SS_OPT_COUNT; ++i)
        if (ss_opt_is_bios(i)) /* the console's BIOS is not the game's: the file neither lists nor changes it */
            out->ss_opt[i] = global->ss_opt[i];
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
    snprintf(out->usb_folder, sizeof(out->usb_folder), "%s", global->usb_folder);
    snprintf(out->nfs_shares, sizeof(out->nfs_shares), "%s", global->nfs_shares);
    out->nfs_timeout = global->nfs_timeout;
    out->nostalgia = global->nostalgia;
    out->quick_resume = global->quick_resume;
    return true;
}

/* One setting read straight from a settings file, before the rest is loaded
 * (frame interpolation's 120 Hz mode is chosen when the screen opens) */
bool config_peek_bool(const char *path, const char *key)
{
    return config_peek_bool_or(path, key, false);
}

/* A text value from a settings file; false when it isn't there */
bool config_peek_string(const char *path, const char *key, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    size_t n = strlen(key);
    bool found = false;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n) && line[n] == '=')
        {
            line[strcspn(line, "\r\n")] = '\0';
            snprintf(out, size, "%s", line + n + 1);
            found = true;
        }
    fclose(f);
    return found;
}

/* A whole number from a settings file, `otherwise` when it isn't there */
int config_peek_int(const char *path, const char *key, int otherwise)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return otherwise;
    char line[256];
    size_t n = strlen(key);
    int value = otherwise;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n) && line[n] == '=')
            value = atoi(line + n + 1);
    fclose(f);
    return value;
}

/* The same, with the value for a file that doesn't have the key (or doesn't exist) */
bool config_peek_bool_or(const char *path, const char *key, bool otherwise)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return otherwise;
    char line[256];
    size_t n = strlen(key);
    bool on = otherwise;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n) && line[n] == '=')
            on = atoi(line + n + 1) != 0;
    fclose(f);
    return on;
}

bool config_save(const Settings *s, const char *path)
{
    char temp[SwanStationPS5_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    if (!f)
        return false;
    fprintf(f,
            "# SwanStationPS5 settings\n"
            "smooth=%d\nshow_fps=%d\nregion=%d\n"
            "dithering=%d\ncd_speed=%d\nanalog=%d\nstate_slot=%d\nlast_game=%d\n"
            "cover_style=%d\ncover_download=%d\nui_sound=%d\nui_volume=%d\n"
            "integer_scale=%d\ninternal_scale=%d\nupscale=%d\nupscale_filter=%d\nstick_dpad=%d\nlanguage=%d\n"
            "vibration=%d\nsort_mode=%d\nshelf_category=%d\nbackground=%d\nmenu_combo=%d\nmenu_hold=%d\n",
            s->smooth, s->show_fps, s->region, s->dithering,
            s->cd_speed, s->analog, s->state_slot, s->last_game, s->cover_style,
            s->cover_download, s->ui_sound, s->ui_volume, s->integer_scale, s->internal_res, s->upscale,
            s->upscale_filter, s->stick_dpad, s->language, s->vibration,
            s->sort_mode, s->shelf_category, s->background, s->menu_combo, s->menu_hold);
    fprintf(f, "widescreen=%d\nmultitap=%d\nrewind=%d\nquick_resume=%d\ncrt=%d\nborder=%d\nremote=%d\nupdate_check=%d\n",
            s->widescreen, s->multitap, s->rewind, s->quick_resume, s->crt, s->border, s->remote,
            s->update_check);
    fprintf(f, "ntfs_enabled=%d\n", s->ntfs_enabled);
    fprintf(f, "renderer=%d\nusb_folder=%s\nnfs_timeout=%d\nnfs_shares=%s\n", s->renderer, s->usb_folder, s->nfs_timeout, s->nfs_shares);
    fprintf(f, "version=%s\n", SwanStationPS5_VERSION); /* a newer version starts with clean settings (main.c) */
    fprintf(f, "pgxp=%d\nra_popups=%d\nra_tracker=%d\ncrop_edges=%d\n", s->pgxp, s->ra_popups, s->ra_tracker,
            s->crop_edges);
    fprintf(f, "manual_crop=%d\nmc_w=%d\nmc_h=%d\nmc_x=%d\nmc_y=%d\n", s->manual_crop, s->mc_w, s->mc_h, s->mc_x, s->mc_y);
    fprintf(f, "ra_popup_style=%d\nlightbar=%d\nshader=%d\nhd_textures=%d\ntheme=%d\n", s->ra_popup_style,
            s->lightbar, s->shader, s->hd_textures, s->theme);
    fprintf(f, "stick_deadzone=%d\nstick_response=%d\ntrigger_effects=%d\nracing_triggers=%d\nrumble_feel=%d\n",
            s->stick_deadzone, s->stick_response, s->trigger_effects, s->racing_triggers, s->rumble_feel);
    fprintf(f, "lightgun=%d\noverclock=%d\nbrightness=%d\ncolour=%d\nautosave=%d\nbezel=%d\n", s->lightgun,
            s->overclock, s->brightness, s->colour, s->autosave, s->bezel);
    fprintf(f, "msaa=%d\ntexture_filter=%d\nfilter_2d=%d\nsupersampling=%d\ndeinterlace=%d\npal60=%d\ngame_fixes=%d\n",
            s->msaa, s->texture_filter, s->filter_2d, s->supersampling, s->deinterlace, s->pal60, s->game_fixes);
    fprintf(f, "fmv_smooth=%d\ntrue_colour=%d\nboot_intro=%d\nsharpen=%d\ndisc_animation=%d\ndisc_in_tray=%d\nnegcon=%d\njewel_cases=%d\ncase_lighting=%d\ndisc_ask=%d\ninstall_to_ext=%d\n",
            s->fmv_smooth, s->true_colour, s->boot_intro, s->sharpen, s->disc_animation, s->disc_in_tray, s->negcon, s->jewel_cases, s->case_lighting, s->disc_ask, s->install_to_ext);
    fprintf(f, "touch_mouse=%d\nrun_ahead=%d\nfast_effects=%d\nframegen=%d\nvideo_out=%d\nfsr=%d\nframe_pacing=%d\nnostalgia=%d\n", s->touch_mouse, s->run_ahead,
            s->fast_effects, s->framegen, s->video_out, s->fsr, s->frame_pacing, s->nostalgia);
    for (int i = 0; i < SS_OPT_COUNT; ++i)
    {
        int v = s->ss_opt[i];
        if (v >= 0 && v < ss_opt_count(i) && v != SS_OPTS[i].def) /* only what differs from the default */
            fprintf(f, "%s=%s\n", SS_OPTS[i].key, ss_opt_value(i, v));
    }
    for (int i = 0; i < SS_OPT_COUNT && s->aspect_before_wide >= 0; ++i)
        if (strcmp(SS_OPTS[i].key, "swanstation_Display_AspectRatio") == 0 && s->aspect_before_wide < SS_OPTS[i].count)
            fprintf(f, "aspect_before_widescreen=%s\n", SS_OPTS[i].values[s->aspect_before_wide]);
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
    path_join(p->cache, sizeof(p->cache), root, "cache");
    path_join(p->bios, sizeof(p->bios), root, "bios");
    path_join(p->saves, sizeof(p->saves), root, "saves");
    path_join(p->states, sizeof(p->states), root, "states");
    path_join(p->cheats, sizeof(p->cheats), root, "cheats");
    path_join(p->covers, sizeof(p->covers), root, "covers");
    path_join(p->logs, sizeof(p->logs), root, "logs");
    path_join(p->config, sizeof(p->config), root, "SwanStationPS5.ini");
    str_copy(p->user, sizeof(p->user), root);
}

void config_user_paths(Paths *p, const char *dir)
{
    str_copy(p->user, sizeof(p->user), dir);
    path_join(p->saves, sizeof(p->saves), dir, "saves");
    path_join(p->states, sizeof(p->states), dir, "states");
    path_join(p->config, sizeof(p->config), dir, "SwanStationPS5.ini");
}
