/*
 * SwanStationPS5 - RetroAchievements sign-in on the console: user name and password
 * typed with an on-screen keyboard drawn by the app (the system keyboard
 * module stops the title from launching, see platform/ps5_shims.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The password goes to retroachievements.org and nowhere else; the console
 * keeps only the login token it gets back, and the password is wiped.
 */
#include "../app.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "../ra/achievements.h"
#include "../ra/login.h"
#include "coverflow.h"
#include "draw.h"
#include "icons.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CENTER_X (plat_width() * 0.5f)
#define USER_MAX 40
#define PASS_MAX 100

enum { IDLE, BUSY, DONE_OK, DONE_FAIL };

/* the keys: letters and digits, or symbols; the last row is Space and Sign in */
static const char *const PAGE_LETTERS[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-@"};
static const char *const PAGE_SYMBOLS[4] = {"!@#$%^&*()", "-_=+[]{};:", "'\",.<>/?\\|", "~`"};
#define KEY_ROWS 4

static struct
{
    enum Screen back_to;
    char user[USER_MAX + 1], pass[PASS_MAX + 1];
    int field;      /* 0 user name, 1 password */
    int row, col;   /* row KEY_ROWS is Space (col 0) and Sign in (col 1) */
    bool shift, symbols;
    char error[160];
} L;

/* the request runs in a thread; the main thread picks the result up */
static SDL_atomic_t state;
static char job_user[USER_MAX + 1], job_pass[PASS_MAX + 1];
static char job_token[128], job_who[64], job_error[160];

void login_open(enum Screen back_to)
{
    L.back_to = back_to;
    L.user[0] = L.pass[0] = L.error[0] = '\0';
    L.field = 0;
    L.row = 1;
    L.col = 0;
    L.shift = L.symbols = false;
    SDL_AtomicSet(&state, IDLE);
    app.screen = SCREEN_RA_LOGIN;
}

static int login_thread(void *unused)
{
    (void)unused;
    bool ok = ra_password_login(job_user, job_pass, job_token, sizeof(job_token), job_who, sizeof(job_who),
                                job_error, sizeof(job_error));
    memset(job_pass, 0, sizeof(job_pass)); /* the password is not kept */
    SDL_AtomicSet(&state, ok ? DONE_OK : DONE_FAIL);
    return 0;
}

static void submit(void)
{
    if (SDL_AtomicGet(&state) == BUSY)
        return;
    if (!L.user[0] || !L.pass[0])
    {
        str_copy(L.error, sizeof(L.error), tr("Enter your user name and password"));
        sfx_play(SFX_BACK);
        return;
    }
    L.error[0] = '\0';
    str_copy(job_user, sizeof(job_user), L.user);
    str_copy(job_pass, sizeof(job_pass), L.pass);
    SDL_AtomicSet(&state, BUSY);
    SDL_Thread *t = SDL_CreateThread(login_thread, "ra-login", NULL);
    if (t)
        SDL_DetachThread(t);
    else
    {
        memset(job_pass, 0, sizeof(job_pass));
        SDL_AtomicSet(&state, IDLE);
        str_copy(L.error, sizeof(L.error), tr("Couldn't reach RetroAchievements"));
    }
    sfx_play(SFX_SELECT);
}

static int row_length(int row)
{
    return row == KEY_ROWS ? 2 : (int)strlen((L.symbols ? PAGE_SYMBOLS : PAGE_LETTERS)[row]);
}

static char key_at(int row, int col)
{
    char c = (L.symbols ? PAGE_SYMBOLS : PAGE_LETTERS)[row][col];
    return L.shift && !L.symbols ? (char)toupper((unsigned char)c) : c;
}

static void type(char c)
{
    char *text = L.field ? L.pass : L.user;
    size_t max = L.field ? PASS_MAX : USER_MAX, n = strlen(text);
    if (n < max)
    {
        text[n] = c;
        text[n + 1] = '\0';
    }
    L.error[0] = '\0';
}

static void erase(void)
{
    char *text = L.field ? L.pass : L.user;
    size_t n = strlen(text);
    if (n)
        text[n - 1] = '\0';
}

static void wipe_and_leave(void)
{
    memset(L.pass, 0, sizeof(L.pass));
    memset(job_pass, 0, sizeof(job_pass));
    app.screen = L.back_to;
}

static void draw_field(float x, float y, float w, const char *label, const char *text, bool active)
{
    draw_rrect(x, y, w, 84, TH_RADIUS_SMALL, active ? TH_ROW_SELECTED : TH_CARD);
    if (active)
        draw_rrect_outline(x, y, w, 84, TH_RADIUS_SMALL, 3, TH_FOCUS);
    text_draw(x + 24, y + 8, 20, FONT_REGULAR, TH_TEXT_DIM, ALIGN_LEFT, label);
    char shown[PASS_MAX + 2];
    str_copy(shown, sizeof(shown), text); /* the password is always visible */
    if (active)
        str_copy(shown + strlen(shown), sizeof(shown) - strlen(shown), "_");
    text_draw_fit(x + 24, y + 36, 32, FONT_BOLD, TH_TEXT, ALIGN_LEFT, w - 48, shown);
}

void login_screen(uint32_t pressed)
{
    int st = SDL_AtomicGet(&state);
    if (st == DONE_OK)
    {
        char who[64], token[128];
        str_copy(who, sizeof(who), job_who);
        str_copy(token, sizeof(token), job_token);
        memset(job_token, 0, sizeof(job_token));
        SDL_AtomicSet(&state, IDLE);
        ra_use_token(who, token);
        memset(token, 0, sizeof(token));
        char msg[96];
        snprintf(msg, sizeof(msg), tr("Signed in as %s"), who);
        wipe_and_leave();
        app_toast(msg);
        sfx_play(SFX_SELECT);
        profile_open(L.back_to);
        return;
    }
    if (st == DONE_FAIL)
    {
        str_copy(L.error, sizeof(L.error), job_error[0] ? job_error : "Sign-in failed");
        SDL_AtomicSet(&state, IDLE);
        sfx_play(SFX_BACK);
        st = IDLE;
    }
    const bool busy = st == BUSY;

    if (!busy)
    {
        int row = L.row, col = L.col;
        if (pressed & BIT(BTN_UP))
            L.row = (L.row + KEY_ROWS) % (KEY_ROWS + 1);
        if (pressed & BIT(BTN_DOWN))
            L.row = (L.row + 1) % (KEY_ROWS + 1);
        if (pressed & BIT(BTN_LEFT))
            --L.col;
        if (pressed & BIT(BTN_RIGHT))
            ++L.col;
        int len = row_length(L.row);
        if (L.col < 0)
            L.col = len - 1;
        if (L.col >= len)
            L.col = (pressed & BIT(BTN_RIGHT)) ? 0 : len - 1;
        if (L.row != row || L.col != col)
            sfx_play(SFX_CLICK);
        if (pressed & (BIT(BTN_L1) | BIT(BTN_R1)))
        {
            L.field ^= 1;
            sfx_play(SFX_CLICK);
        }
        if (pressed & BIT(BTN_L2))
        {
            L.symbols = !L.symbols;
            if (L.col >= row_length(L.row))
                L.col = row_length(L.row) - 1;
            sfx_play(SFX_CLICK);
        }
        if (pressed & BIT(BTN_TRIANGLE))
        {
            L.shift = !L.shift;
            sfx_play(SFX_CLICK);
        }
        if (pressed & BIT(BTN_SQUARE))
        {
            erase();
            sfx_play(SFX_CLICK);
        }
        if (pressed & BIT(BTN_START))
            submit();
        if (pressed & BIT(BTN_CROSS))
        {
            if (L.row < KEY_ROWS)
            {
                type(key_at(L.row, L.col));
                sfx_play(SFX_CLICK);
            }
            else if (L.col == 0)
            {
                type(' ');
                sfx_play(SFX_CLICK);
            }
            else
                submit();
        }
    }
    if ((pressed & BIT(BTN_CIRCLE)) && !busy)
    {
        sfx_play(SFX_BACK);
        wipe_and_leave();
        return;
    }

    /* ------------------------------------------------ drawing */
    if (app.game)
    {
        app_draw_game(40);
        draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);
    }
    else
        shelf_backdrop();
    text_draw(CENTER_X, 44, 44, FONT_BOLD, TH_TEXT, ALIGN_CENTER, tr("Sign in to RetroAchievements"));
    text_draw_fit(CENTER_X, 104, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, plat_width() - 200,
                  tr("Your password goes only to retroachievements.org; the console keeps just a token."));

    const float fw = 760, fx = CENTER_X - fw * 0.5f;
    draw_field(fx, 160, fw, tr("User name"), L.user, L.field == 0);
    draw_field(fx, 256, fw, tr("Password"), L.pass, L.field == 1);

    {
        const int pad[2] = {GLYPH_L1, GLYPH_R1};
        const char *msg = "Switch between user name and password";
        draw_hint_combo(CENTER_X - hint_combo_width(pad, 2, msg, 20) * 0.5f, 342, pad, 2, '/', msg, 20, TH_TEXT_DIM);
    }

    const float key = 104, gap = 12, kh = 88;
    float ky = 372;
    for (int r = 0; r < KEY_ROWS; ++r)
    {
        int len = row_length(r);
        float x = CENTER_X - (len * (key + gap) - gap) * 0.5f;
        for (int c = 0; c < len; ++c)
        {
            bool on = r == L.row && c == L.col;
            float kx = x + c * (key + gap);
            draw_rrect(kx, ky, key, kh, TH_RADIUS_SMALL, on ? TH_ROW_SELECTED : TH_CARD);
            if (on)
                draw_rrect_outline(kx, ky, key, kh, TH_RADIUS_SMALL, 3, TH_FOCUS);
            char label[2] = {key_at(r, c), '\0'};
            text_draw(kx + key * 0.5f, ky + 20, 38, FONT_BOLD, TH_TEXT, ALIGN_CENTER, label);
        }
        ky += kh + gap;
    }
    {
        const float sw = 5 * (key + gap) - gap, space_w = sw * 0.62f, go_w = sw - space_w - gap;
        const float sx = CENTER_X - sw * 0.5f;
        for (int c = 0; c < 2; ++c)
        {
            bool on = L.row == KEY_ROWS && L.col == c;
            float kx = c == 0 ? sx : sx + space_w + gap, kw = c == 0 ? space_w : go_w;
            draw_rrect(kx, ky, kw, kh, TH_RADIUS_SMALL, on ? TH_ROW_SELECTED : (c ? TH_PILL : TH_CARD));
            if (on)
                draw_rrect_outline(kx, ky, kw, kh, TH_RADIUS_SMALL, 3, TH_FOCUS);
            text_draw(kx + kw * 0.5f, ky + 24, 30, FONT_BOLD, c ? TH_FOCUS : TH_TEXT_DIM, ALIGN_CENTER,
                      c ? tr("Sign in") : tr("Space"));
        }
        ky += kh + gap;
    }
    if (busy)
        text_draw(CENTER_X, ky + 14, 28, FONT_BOLD, TH_FOCUS, ALIGN_CENTER, tr("Signing in..."));
    else if (L.error[0])
        text_draw_fit(CENTER_X, ky + 14, 26, FONT_BOLD, TH_DANGER, ALIGN_CENTER, plat_width() - 200, L.error);

    /* for languages the keyboard cannot type: the PC script signs in instead (hidden while a status shows) */
    if (!busy && !L.error[0])
    {
        text_draw_fit(CENTER_X, ky + 20, 30, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, plat_width() - 160,
                      tr("If your language can't be typed with this keyboard, sign in from your PC:"));
        text_draw(CENTER_X, ky + 60, 30, FONT_BOLD, TH_TEXT_DIM, ALIGN_CENTER, "python tools/SwanStationPS5_sync.py ra-login");
    }

    static const int glyphs[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_TRIANGLE, GLYPH_START, GLYPH_CIRCLE};
    static const char *const labels[] = {"Type", "Delete", "Shift", "Sign in", "Back"};
    app_draw_hints(glyphs, labels, 5, NULL);
    const HintCombo right[2] = {{{GLYPH_L1, GLYPH_R1}, 2, '/', "Field"}, {{GLYPH_L2}, 1, '+', "Symbols"}};
    app_draw_hints_right(right, 2);
    app_draw_toast();
}
