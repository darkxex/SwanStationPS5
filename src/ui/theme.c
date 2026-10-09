/*
 * SwanStationPS5 - the themes: Classic, Neon Arcade, Memory Card (light and dark) and
 * Record Shelf. Settings > Library > Theme.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "theme.h"

#include "text.h"

#include <string.h>

Theme theme;
static int current = -1;

const char *const THEME_NAMES[THEME_COUNT] = {"Classic", "Neon Arcade", "Memory Card", "Memory Card Dark",
                                              "Record Shelf"};

static const Theme THEMES[THEME_COUNT] = {
    [THEME_CLASSIC] = {
        .bg = 0xff0f1330u, .bg_deep = 0xff0a0d24u, .card = 0xff151a3du, .card_soft = 0xd8151a3du,
        .row_selected = 0xff2a3370u, .focus = 0xff8fb0ffu, .pill = 0xff1c2250u, .switch_on = 0xff5b7cffu,
        .switch_off = 0xff3a4280u, .text = 0xffe8ebffu, .text_dim = 0xff8f97c8u, .text_soft = 0xffcfd6ffu,
        .hint = 0xff9aa3d6u, .gold = 0xfff0b429u, .danger = 0xffff9c9cu, .good = 0xff5fd38au,
        .divider = 0xff222a5cu, .cover_outline = 0xffe8ebffu, .backdrop_tint = 0, .backdrop = BACKDROP_STAGE,
        .font_regular = "fonts/Inter-400.ttf", .font_bold = "fonts/Inter-600.ttf"},
    [THEME_NEON] = {
        .bg = 0xff07060fu, .bg_deep = 0xff040309u, .card = 0xff151033u, .card_soft = 0xd8151033u,
        .row_selected = 0xff3b1f5eu, .focus = 0xff00e5ffu, .pill = 0xff1d1640u, .switch_on = 0xffff3fa4u,
        .switch_off = 0xff3a2f6bu, .text = 0xfff2ecffu, .text_dim = 0xffb7a9e0u, .text_soft = 0xffddd2ffu,
        .hint = 0xffb7a9e0u, .gold = 0xffffd23fu, .danger = 0xffff7a9cu, .good = 0xff3ff2a0u,
        .divider = 0xff2a1f55u, .cover_outline = 0xff00e5ffu, .backdrop_tint = 0xffff3fa4u, .backdrop = BACKDROP_GRID,
        .font_regular = "fonts/ChakraPetch-500.ttf", .font_bold = "fonts/ChakraPetch-700.ttf"},
    [THEME_MEMORY] = {
        .bg = 0xfff4f4f6u, .bg_deep = 0xffe9eaeeu, .card = 0xffffffffu, .card_soft = 0xf0ffffffu,
        .row_selected = 0xffdde5f5u, .focus = 0xff2f5fb3u, .pill = 0xffe3e5eau, .switch_on = 0xff2f5fb3u,
        .switch_off = 0xffc4c7cfu, .text = 0xff2b2d33u, .text_dim = 0xff5f636eu, .text_soft = 0xff3d4048u,
        .hint = 0xff5a5e69u, .gold = 0xffb07d0fu, .danger = 0xffc4302bu, .good = 0xff1f8a52u,
        .divider = 0xffd4d6dcu, .cover_outline = 0xff2f5fb3u, .backdrop_tint = 0xfff4f4f6u, .backdrop = BACKDROP_FLAT,
        .layout = LAYOUT_GRID, .light = true, .font_regular = "fonts/IBMPlexSans-400.ttf", .font_bold = "fonts/IBMPlexSans-600.ttf"},
    [THEME_MEMORY_DARK] = {
        .bg = 0xff1f2128u, .bg_deep = 0xff17181eu, .card = 0xff2a2d36u, .card_soft = 0xe02a2d36u,
        .row_selected = 0xff35425eu, .focus = 0xff7fa6ecu, .pill = 0xff33363fu, .switch_on = 0xff4a7bd0u,
        .switch_off = 0xff4a4d58u, .text = 0xffe8e9edu, .text_dim = 0xff9a9eaau, .text_soft = 0xffcfd1d8u,
        .hint = 0xff9a9eaau, .gold = 0xffe9b52au, .danger = 0xffff8a80u, .good = 0xff5fc98au,
        .divider = 0xff3a3d47u, .cover_outline = 0xff7fa6ecu, .backdrop_tint = 0xff8a90a6u, .backdrop = BACKDROP_FLAT,
        .layout = LAYOUT_GRID,
        .font_regular = "fonts/IBMPlexSans-400.ttf", .font_bold = "fonts/IBMPlexSans-600.ttf"},
    [THEME_RECORD] = {
        .bg = 0xff22160fu, .bg_deep = 0xff170f0au, .card = 0xff2e2018u, .card_soft = 0xe02e2018u,
        .row_selected = 0xff4a3324u, .focus = 0xffe8a35au, .pill = 0xff3a281cu, .switch_on = 0xffe8a35au,
        .switch_off = 0xff5a4434u, .text = 0xfff4e7d6u, .text_dim = 0xffb89a7du, .text_soft = 0xffe2cfb8u,
        .hint = 0xffb89a7du, .gold = 0xffe8b85au, .danger = 0xffe88a7au, .good = 0xff9fcf8au,
        .divider = 0xff4a3628u, .cover_outline = 0xffe8a35au, .backdrop_tint = 0xffffc98au, .backdrop = BACKDROP_LAMP,
        .shelf_plank = true, .layout = LAYOUT_SPINES, .font_regular = "fonts/WorkSans-400.ttf", .font_bold = "fonts/Fraunces-600.ttf"},
};

void theme_apply(int id)
{
    if (id < 0 || id >= THEME_COUNT)
        id = THEME_CLASSIC;
    const char *old_regular = current >= 0 ? theme.font_regular : NULL;
    const char *old_bold = current >= 0 ? theme.font_bold : NULL;
    theme = THEMES[id];
    current = id;
    if (old_regular != theme.font_regular || old_bold != theme.font_bold)
        text_set_fonts(theme.font_regular, theme.font_bold);
}

int theme_current(void)
{
    return current;
}
