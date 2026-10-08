/*
 * PSXS5 - what PSXS5 does with the controller while a game runs.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The light gun: the DualSense's orientation (from its motion sensor) is
 * turned into a point on the picture, relative to where the controller was
 * pointing when the game started (R3 re-centres). Without the sensor, the
 * right stick moves the crosshair.
 */
#include "controls.h"

#include "cheats.h"
#include "core/host.h"
#include "gamedb.h"
#include "platform/platform.h"
#include "ui/draw.h"
#include "ui/theme.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- which games */

static char *kinds;
static bool kinds_tried;

static void load_kinds(void)
{
    if (kinds_tried)
        return;
    kinds_tried = true;
    char path[PSXS5_PATH_MAX];
    plat_asset_path(path, sizeof(path), "game-kinds.txt");
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    kinds = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (kinds)
        kinds[fread(kinds, 1, (size_t)size, f)] = '\0';
    fclose(f);
}

/* lower-case words of letters and digits */
static int split_words(const char *s, char out[][24], int max)
{
    int n = 0;
    while (*s && n < max)
    {
        while (*s && !isalnum((unsigned char)*s))
            ++s;
        int len = 0;
        while (*s && isalnum((unsigned char)*s))
        {
            if (len < 23)
                out[n][len++] = (char)tolower((unsigned char)*s);
            ++s;
        }
        if (len)
            out[n++][len] = '\0';
    }
    return n;
}

/* every word of the series name is in the title */
static bool series_match(const char *series, const char *title)
{
    char a[16][24], b[24][24];
    int na = split_words(series, a, 16), nb = split_words(title, b, 24);
    if (!na)
        return false;
    for (int i = 0; i < na; ++i)
    {
        bool found = false;
        for (int j = 0; j < nb && !found; ++j)
            found = strcmp(a[i], b[j]) == 0;
        if (!found)
            return false;
    }
    return true;
}

int controls_kind(const Game *g)
{
    load_kinds();
    if (!g)
        return 0;
    int found = 0;
    /* DuckStation's database: which controllers the disc takes */
    const GameInfo *info = gamedb_get(g->serial);
    if (info)
    {
        if (info->flags & GDB_JUSTIFIER)
            found |= KIND_JUSTIFIER;
        else if (info->flags & GDB_GUNCON)
            found |= KIND_GUNCON;
        if ((info->flags & GDB_NEGCON) || strstr(info->genre, "Racing") || strstr(info->genre, "Driving"))
            found |= KIND_PEDAL;
    }
    if (!kinds)
        return found;
    bool listed_gun = false;
    for (const char *line = kinds; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
    {
        if (*line == '#' || *line == '\n' || *line == '\r')
            continue;
        char buf[200];
        size_t len = strcspn(line, "\r\n");
        if (len >= sizeof(buf))
            continue;
        memcpy(buf, line, len);
        buf[len] = '\0';
        char *title = strchr(buf, '\t');
        if (!title)
            continue;
        *title++ = '\0';
        int kind = !strcmp(buf, "guncon") ? KIND_GUNCON : !strcmp(buf, "justifier") ? KIND_JUSTIFIER
                   : !strcmp(buf, "racing")  ? KIND_RACING
                                             : 0;
        size_t tl = strlen(title);
        bool hit;
        if (tl && title[tl - 1] == '*')
        {
            title[tl - 1] = '\0';
            hit = series_match(title, g->title) || series_match(title, g->disc_name);
        }
        else
            hit = titles_match(title, g->title) || titles_match(title, g->disc_name);
        if (hit)
        {
            /* the list's gun beats the database's; the first entry wins ("Die Hard
             * Trilogy 2" before "Die Hard Trilogy") */
            if (kind & (KIND_GUNCON | KIND_JUSTIFIER))
            {
                if (!listed_gun)
                    found = (found & ~(KIND_GUNCON | KIND_JUSTIFIER)) | kind;
                listed_gun = true;
            }
            else
                found |= kind;
        }
    }
    return found;
}

int controls_special_for(const Game *g, const Settings *s)
{
    if (controls_gun_for(g, s))
        return 0;
    const GameInfo *info = gamedb_get(g->serial);
    if (!info)
        return 0;
    if (s->negcon && (info->flags & GDB_NEGCON))
        return 1;
    if (s->touch_mouse && (info->flags & GDB_MOUSE))
        return 2;
    return 0;
}

int controls_gun_for(const Game *g, const Settings *s)
{
    if (s->lightgun == 1)
        return 0;
    int k = controls_kind(g);
    if (k & KIND_JUSTIFIER)
        return 2;
    if ((k & KIND_GUNCON) || s->lightgun == 2)
        return 1;
    return 0;
}

/* ---------------------------------------------------------------- state */

static struct
{
    int gun;           /* 0 none, 1 GunCon, 2 Justifier: player 1 holds it */
    bool racing;       /* Cross is the gas and Square the brake: they can go on R2 / L2 */
    bool pedal;        /* a racing game: R2 feels like a pedal */
    int special;       /* 1 NeGcon (analog gas and brake), 2 a mouse on the touchpad */
    bool was_touching;
    uint16_t last_x, last_y;
    bool centred;      /* centre holds the orientation that aims at the middle */
    float centre[4];
    float x, y;        /* the aim, -1..1 across the picture */
    bool offscreen;
    uint32_t prev;     /* player 1's buttons last frame */
} C;

void controls_start(const Game *g, const Settings *s)
{
    memset(&C, 0, sizeof(C));
    C.gun = controls_gun_for(g, s);
    int kind = controls_kind(g);
    C.racing = (kind & KIND_RACING) != 0;
    C.pedal = (kind & (KIND_RACING | KIND_PEDAL)) != 0;
    C.special = controls_special_for(g, s);
    plat_pad_motion(C.gun != 0);
    plat_pad_touch(C.special == 2);
    if (C.gun || C.pedal)
        psxs5_log("controls: %s%s", C.gun ? (C.gun == 1 ? "GunCon " : "Justifier ") : "",
                  C.racing ? "racing" : C.pedal ? "pedal" : "");
}

void controls_stop(void)
{
    PlatTrigger off = {0};
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
        plat_pad_triggers(i, off, off);
    plat_pad_motion(false);
    plat_pad_touch(false);
    memset(&C, 0, sizeof(C));
}

bool controls_gun_active(void)
{
    return C.gun != 0;
}

void controls_recenter(void)
{
    C.centred = false;
    C.x = C.y = 0;
}

/* ---------------------------------------------------------------- sticks */

static int16_t clamp16(float v)
{
    return (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
}

static void shape_stick(int16_t *x, int16_t *y, int deadzone, int response)
{
    if (!deadzone && !response)
        return;
    float fx = *x / 32767.0f, fy = *y / 32767.0f, m = sqrtf(fx * fx + fy * fy);
    float dz = deadzone * 0.05f;
    if (m <= dz || m < 0.0001f)
    {
        *x = *y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz);
    if (k > 1.0f)
        k = 1.0f;
    if (response == 1)
        k = powf(k, 1.8f); /* precise: small moves stay small */
    else if (response == 2)
        k = powf(k, 0.6f); /* quick: a little tilt goes far */
    float scale = k / m * 32767.0f;
    *x = clamp16(fx * scale);
    *y = clamp16(fy * scale);
}

/* ---------------------------------------------------------------- the gun */

static void quat_mul(const float a[4], const float b[4], float out[4])
{
    /* x, y, z, w */
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* Where the controller points, relative to the centre: the motion sensor's
 * axes are x right, y up, z towards the player, so it points along -z. */
static void aim_from_motion(const float q[4], float *ax, float *ay)
{
    float inv[4] = {-C.centre[0], -C.centre[1], -C.centre[2], C.centre[3]}, r[4];
    quat_mul(inv, q, r);
    /* v = r * (0, 0, -1) * conj(r) */
    float x = r[0], y = r[1], z = r[2], w = r[3];
    float vx = -(2.0f * (x * z + w * y));
    float vy = -(2.0f * (y * z - w * x));
    float vz = -(1.0f - 2.0f * (x * x + y * y));
    float yaw = atan2f(vx, -vz);
    float pitch = asinf(vy < -1.0f ? -1.0f : vy > 1.0f ? 1.0f : vy);
    const float yaw_range = 0.42f, pitch_range = 0.26f; /* about 24 and 15 degrees to the edges */
    *ax = yaw / yaw_range;
    *ay = -pitch / pitch_range;
}

static void update_gun(PadState *p, float dt)
{
    uint32_t newly = p->buttons & ~C.prev;
    C.prev = p->buttons;
    if (newly & BIT(BTN_R3))
        controls_recenter();
    float tx, ty;
    if (p->motion)
    {
        if (!C.centred)
        {
            memcpy(C.centre, p->quat, sizeof(C.centre));
            C.centred = true;
        }
        aim_from_motion(p->quat, &tx, &ty);
        /* a little smoothing against the sensor's jitter */
        float k = dt * 40.0f;
        if (k > 1.0f)
            k = 1.0f;
        C.x += (tx - C.x) * k;
        C.y += (ty - C.y) * k;
    }
    else
    {
        /* no motion sensor: the right stick moves the crosshair */
        float sx = p->rx / 32767.0f, sy = p->ry / 32767.0f;
        if (fabsf(sx) < 0.15f)
            sx = 0;
        if (fabsf(sy) < 0.15f)
            sy = 0;
        C.x += sx * dt * 1.6f;
        C.y += sy * dt * 1.6f;
        C.x = C.x < -1.0f ? -1.0f : C.x > 1.0f ? 1.0f : C.x;
        C.y = C.y < -1.0f ? -1.0f : C.y > 1.0f ? 1.0f : C.y;
    }
    C.offscreen = fabsf(C.x) > 1.0f || fabsf(C.y) > 1.0f;
    p->gun_x = clamp16(C.x * 32767.0f);
    p->gun_y = clamp16(C.y * 32767.0f);
    p->gun_offscreen = C.offscreen;
}

/* ---------------------------------------------------------------- each frame */

static void update_triggers(int port, const Settings *s)
{
    PlatTrigger l2 = {0}, r2 = {0};
    float rumble = host_rumble_level(port);
    if (s->vibration && s->rumble_feel == 3 && rumble > 0.05f)
    {
        /* the game's rumble in the triggers too */
        PlatTrigger v = {TRIGGER_VIBRATION, 2, (uint8_t)(1 + rumble * 7.0f), (uint8_t)(30 + rumble * 120.0f), 0};
        l2 = r2 = v;
    }
    else if (s->trigger_effects && C.gun && port == 0)
    {
        PlatTrigger click = {TRIGGER_WEAPON, 3, 6, 6, 0}; /* a gun's trigger: resists, then gives */
        r2 = click;
    }
    else if (s->trigger_effects && C.pedal)
    {
        PlatTrigger pedal = {TRIGGER_SLOPE, 1, 9, 2, 6};  /* firmer the further it goes */
        PlatTrigger brake = {TRIGGER_FEEDBACK, 2, 6, 0, 0};
        r2 = pedal;
        l2 = brake;
    }
    plat_pad_triggers(port, l2, r2);
}

void controls_apply(PadState pads[PSXS5_MAX_PADS], const Settings *s, float dt)
{
    for (int i = 0; i < PSXS5_MAX_PADS; ++i)
    {
        PadState *p = &pads[i];
        if (!p->connected)
            continue;
        shape_stick(&p->lx, &p->ly, s->stick_deadzone, s->stick_response);
        shape_stick(&p->rx, &p->ry, s->stick_deadzone, s->stick_response);
        /* with a NeGcon the triggers are analog already: no digital remap */
        if (C.racing && s->racing_triggers && C.special != 1 && !(C.gun && i == 0))
        {
            if (p->buttons & BIT(BTN_R2))
                p->buttons = (p->buttons & ~BIT(BTN_R2)) | BIT(BTN_CROSS);
            if (p->buttons & BIT(BTN_L2))
                p->buttons = (p->buttons & ~BIT(BTN_L2)) | BIT(BTN_SQUARE);
        }
        if (C.gun && i == 0)
            update_gun(p, dt);
        if (C.special == 2 && i == 0)
        {
            /* the touchpad moves the pointer by how far the finger slid; the
             * right stick works too */
            float dx = 0, dy = 0;
            if (p->touching && C.was_touching)
            {
                dx = ((int)p->touch_x - (int)C.last_x) * 0.4f;
                dy = ((int)p->touch_y - (int)C.last_y) * 0.4f;
            }
            C.was_touching = p->touching;
            C.last_x = p->touch_x;
            C.last_y = p->touch_y;
            float sx = p->rx / 32767.0f, sy = p->ry / 32767.0f;
            if (fabsf(sx) > 0.15f)
                dx += sx * dt * 500.0f;
            if (fabsf(sy) > 0.15f)
                dy += sy * dt * 500.0f;
            p->mouse_dx = (int16_t)(dx > 127 ? 127 : dx < -127 ? -127 : dx);
            p->mouse_dy = (int16_t)(dy > 127 ? 127 : dy < -127 ? -127 : dy);
        }
        update_triggers(i, s);
    }
}

void controls_draw(void)
{
    if (!C.gun || C.offscreen)
        return;
    int gx, gy, gw, gh;
    plat_game_rect(&gx, &gy, &gw, &gh);
    if (gw <= 0 || gh <= 0)
        return;
    float cx = gx + (C.x + 1.0f) * 0.5f * gw, cy = gy + (C.y + 1.0f) * 0.5f * gh;
    const uint32_t shade = 0x90000000u, ink = 0xffff4040u;
    draw_ring(cx, cy, 20, 6, shade);
    draw_ring(cx, cy, 20, 3, ink);
    for (int k = 0; k < 2; ++k)
    {
        float t = k ? 3.0f : 6.0f;
        uint32_t c = k ? ink : shade;
        draw_line(cx - 34, cy, cx - 12, cy, t, c);
        draw_line(cx + 12, cy, cx + 34, cy, t, c);
        draw_line(cx, cy - 34, cx, cy - 12, t, c);
        draw_line(cx, cy + 12, cx, cy + 34, t, c);
    }
    draw_circle(cx, cy, 3, ink);
}
