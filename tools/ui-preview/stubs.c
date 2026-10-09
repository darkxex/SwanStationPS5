#include <stdio.h>
/*
 * SwanStationPS5 interface preview - stand-ins for the emulator and RetroAchievements,
 * so the screens run on a PC with a fake game picture.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../src/core/host.h"
#include "../../src/ra/achievements.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ---- the core: a striped test picture instead of a game */
static bool loaded;
static uint32_t frame[320 * 240];

bool host_load(const char *game_path, const char *serial, const Paths *paths, const Settings *settings,
               char *error, size_t error_size)
{
    (void)game_path;
    (void)serial; (void)paths; (void)settings; (void)error; (void)error_size;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
        {
            uint32_t sky = 0xff203a8au + (uint32_t)(y / 4) * 0x010203u;
            bool ground = y > 160, block = ((x / 32) + (y / 32)) % 2 == 0;
            frame[y * 320 + x] = ground ? (block ? 0xff3f8a3fu : 0xff2f6a2fu) : sky;
            if ((x - 230) * (x - 230) + (y - 70) * (y - 70) < 500)
                frame[y * 320 + x] = 0xfff0d050u;
        }
    loaded = true;
    return true;
}
void host_unload(void) { loaded = false; }
bool host_loaded(void) { return loaded; }
const char *host_core_name(void) { return "SwanStation"; }
const char *host_emulator_for(const Settings *s, const char *serial, const char **why)
{ (void)s; (void)serial; *why = NULL; return "Beetle PSX HW"; }
bool host_hash_disc_begin(const char *p) { (void)p; return false; }
void host_hash_disc_end(void) {}
void *host_memory_data(unsigned id) { (void)id; return NULL; }
size_t host_memory_size(unsigned id) { (void)id; return 0; }
void host_set_pads(const PadState pads[SwanStationPS5_MAX_PADS]) { (void)pads; }
void host_set_gun(int device) { (void)device; }
void host_set_special(int device) { (void)device; }
void host_set_fixes(unsigned flags) { (void)flags; }
float host_rumble_level(int port) { (void)port; return 0.0f; }
bool host_pad_digital(int port) { (void)port; return false; }
void host_run_frame(void) {}
void host_set_speculative(bool on) { (void)on; }
void host_reset(void) {}
void host_apply_settings(const Settings *settings) { (void)settings; }
double host_fps(void) { return 59.94; }
int host_sample_rate(void) { return 44100; }
float host_aspect(void) { return 4.0f / 3.0f; }
const void *host_frame(int *width, int *height, size_t *pitch, int *pixel_format, bool *fresh)
{
    *width = 320;
    *height = 240;
    *pitch = 320 * 4;
    *pixel_format = 1;
    *fresh = true;
    return loaded ? frame : NULL;
}
bool host_save_state(const char *path) { (void)path; return true; }
bool host_load_state(const char *path) { (void)path; return true; }
const struct retro_memory_map *host_memory_map(void) { return NULL; }
bool host_read_sector(uint32_t lba, uint8_t out[2048]) { (void)lba; (void)out; return false; }
int host_disc_count(void) { return 2; }
int host_disc_index(void) { return 0; }
bool host_disc_select(int index) { (void)index; return true; }

void retro_cheat_reset(void) {}
void host_cheat_reset(void) {}
void host_beetle_widescreen(bool on) { (void)on; }
void host_cheat_set(unsigned index, const char *code) { (void)index, (void)code; }
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

/* ---- RetroAchievements: a signed-in account with a game in progress */
static bool hardcore;
void ra_init(const Paths *paths) { (void)paths; }
void ra_shutdown(void) {}
void ra_game_loaded(const char *game_path) {}
void ra_game_unloaded(void) {}
void ra_frame(void) {}
void ra_idle(void) {}
void ra_reset(void) {}
bool ra_signed_in(void) { return true; }
const char *ra_user(void) { return "SynoPiia"; }
unsigned ra_user_score(void) { return 1240; }
void ra_use_token(const char *u, const char *t) { (void)u; (void)t; }
void ra_sign_out(void) {}
unsigned ra_user_hardcore_score(void) { return 1240; }
unsigned ra_user_softcore_score(void) { return 85; }
int ra_recent(RaRecent *out, int max)
{
    static const char *const t[] = {"Wumpa Collector", "Dragon Rescuer", "Bonus Round", "Gem Hunter"};
    int n = max < 4 ? max : 4;
    for (int i = 0; i < n; ++i)
    {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].when = (long long)time(NULL) - i * 90000;
        out[i].points = 5 + i * 5;
        snprintf(out[i].game, sizeof(out[i].game), "%s", i < 2 ? "Crash Bandicoot" : "Spyro the Dragon");
        snprintf(out[i].title, sizeof(out[i].title), "%s", t[i]);
    }
    return n;
}
bool ra_game_progress(int *unlocked, int *total)
{
    if (!loaded)
        return false;
    *unlocked = 3;
    *total = 12;
    return true;
}
bool ra_hardcore(void) { return hardcore; }
void ra_set_hardcore(bool on) { hardcore = on; }
void ra_game_summary(char *out, size_t size) { if (size) out[0] = '\0'; }
bool ra_next_message(char *title, size_t title_size, char *detail, size_t detail_size)
{
    static bool shown;
    const char *banner = getenv("SwanStationPS5_BANNER");
    if (shown || !loaded || !banner)
        return false;
    shown = true;
    strncpy(title, "Wumpa Collector", title_size - 1);
    strncpy(detail, "Collect 100 Wumpa fruit in one level  (10 points)", detail_size - 1);
    return true;
}

bool ra_tracker(char *title, size_t ts, char *progress, size_t ps, float *percent)
{
    snprintf(title, ts, "Dragon Rescuer");
    snprintf(progress, ps, "18/80");
    *percent = 22.5f;
    return true;
}

int ra_list(RaAchievement *out, int max)
{
    static const char *const names[][2] = {
        {"Wumpa Collector", "Collect 100 Wumpa fruit in one level"},
        {"Box Breaker", "Break every box in N. Sanity Beach"},
        {"Bonus Round", "Reach a bonus round"},
        {"Gem Hunter", "Collect your first clear gem"},
        {"No Mask Needed", "Finish a level without an Aku Aku mask"},
        {"Boss Fight", "Defeat Papu Papu"},
        {"Island Hopper", "Reach the second island"},
        {"Perfect Run", "Finish a level without dying"},
    };
    int n = 0;
    for (int i = 0; i < 8 && n < max; ++i, ++n)
    {
        RaAchievement *r = &out[n];
        memset(r, 0, sizeof(*r));
        strncpy(r->title, names[i][0], sizeof(r->title) - 1);
        strncpy(r->description, names[i][1], sizeof(r->description) - 1);
        r->points = (unsigned)(5 + i * 5);
        r->unlocked = i < 3;
        if (i == 3)
            strcpy(r->progress, "1/3"), r->percent = 33.3f;
        if (i == 6)
            strcpy(r->progress, "18/80"), r->percent = 22.5f;
    }
    return n;
}

void host_set_patches_dir(const char *dir) { (void)dir; }
size_t host_state_size(void) { return loaded ? 64 : 0; }
bool host_serialize(void *buffer, size_t size) { memset(buffer, 0, size); return loaded; }
bool host_unserialize(const void *buffer, size_t size) { (void)buffer; (void)size; return loaded; }
bool host_capture(uint8_t *rgba, int w, int h)
{
    if (!loaded)
        return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint32_t c = frame[(y * 240 / h) * 320 + x * 320 / w];
            uint8_t *o = &rgba[(y * w + x) * 4];
            o[0] = (c >> 16) & 0xff, o[1] = (c >> 8) & 0xff, o[2] = c & 0xff, o[3] = 255;
        }
    return true;
}
