/*
 * PSXS5 - shape helpers built on plat_draw_mesh.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "draw.h"

#include "../platform/platform.h"
#include "text.h"
#include "icons.h"
#include "theme.h"
#include "../i18n.h"

#include <math.h>
#include <stdlib.h>

uint32_t argb_alpha(uint32_t argb, float alpha)
{
    if (alpha <= 0.0f)
        return argb & 0x00ffffffu;
    if (alpha > 1.0f)
        alpha = 1.0f;
    uint32_t a = (uint32_t)((argb >> 24) * alpha + 0.5f);
    return (argb & 0x00ffffffu) | (a << 24);
}

static void quad(PlatVertex *v, float x0, float y0, float x1, float y1, float x2, float y2,
                 float x3, float y3, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
    /* corners in order: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left */
    PlatVertex a = {x0, y0, 0, 0, c0}, b = {x1, y1, 1, 0, c1}, c = {x2, y2, 1, 1, c2},
               d = {x3, y3, 0, 1, c3};
    v[0] = a; v[1] = b; v[2] = c;
    v[3] = a; v[4] = c; v[5] = d;
}

void draw_rect(float x, float y, float w, float h, uint32_t argb)
{
    plat_fill_rectf(x, y, w, h, argb);
}

uint32_t argb_lerp(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.0f)
        return a;
    if (t >= 1.0f)
        return b;
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
    {
        float ca = (float)((a >> s) & 0xff), cb = (float)((b >> s) & 0xff);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

void anim_approach(float *v, float target, float dt, float speed)
{
    float k = 1.0f - expf(-dt * speed);
    *v += (target - *v) * k;
    if (fabsf(target - *v) < 0.002f * (fabsf(target) + 1.0f))
        *v = target;
}

int list_top_row(int top, int cursor, int rows, int count)
{
    if (cursor < top)
        top = cursor;
    if (cursor >= top + rows)
        top = cursor - rows + 1;
    if (top > count - rows)
        top = count - rows;
    return top < 0 ? 0 : top;
}

/* ---------------------------------------------------------------- rounded shapes */

/* A white anti-aliased disc (128 px): its quarters are the corners of every
 * rounded rectangle. Rings for outlines are cached per (radius, thickness). */
#define DISC 128
static PlatTexture *disc_texture(void)
{
    static PlatTexture *tex;
    static bool tried;
    if (tex || tried)
        return tex;
    tried = true;
    uint8_t *px = malloc(DISC * DISC * 4);
    if (!px)
        return NULL;
    const float c = DISC * 0.5f;
    for (int y = 0; y < DISC; ++y)
        for (int x = 0; x < DISC; ++x)
        {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float a = c - sqrtf(dx * dx + dy * dy) + 0.5f;
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            uint8_t *p = &px[(y * DISC + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    tex = plat_texture_create(px, DISC, DISC, true);
    free(px);
    return tex;
}

typedef struct
{
    int radius, thickness;
    PlatTexture *tex;
} Ring;

static PlatTexture *ring_texture(int radius, int thickness)
{
    static Ring rings[24];
    static int count;
    for (int i = 0; i < count; ++i)
        if (rings[i].radius == radius && rings[i].thickness == thickness)
            return rings[i].tex;
    int size = radius * 2;
    if (size <= 0 || size > 512)
        return NULL;
    uint8_t *px = malloc((size_t)size * size * 4);
    if (!px)
        return NULL;
    const float c = (float)radius;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float dx = x + 0.5f - c, dy = y + 0.5f - c, d = sqrtf(dx * dx + dy * dy);
            float outer = c - d + 0.5f, inner = d - (c - thickness) + 0.5f;
            float a = outer < inner ? outer : inner;
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            uint8_t *p = &px[((size_t)y * size + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    PlatTexture *tex = plat_texture_create(px, size, size, false);
    free(px);
    if (count < 24)
        rings[count++] = (Ring){radius, thickness, tex};
    return tex;
}

void draw_rrect(float x, float y, float w, float h, float r, uint32_t argb)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0)
        return;
    if (r > w * 0.5f)
        r = w * 0.5f;
    if (r > h * 0.5f)
        r = h * 0.5f;
    r = floorf(r + 0.5f);
    PlatTexture *disc = disc_texture();
    if (r < 1.0f || !disc)
    {
        plat_fill_rectf(x, y, w, h, argb);
        return;
    }
    const int q = DISC / 2;
    plat_draw_texture_region(disc, 0, 0, q, q, x, y, r, r, argb);
    plat_draw_texture_region(disc, q, 0, q, q, x + w - r, y, r, r, argb);
    plat_draw_texture_region(disc, 0, q, q, q, x, y + h - r, r, r, argb);
    plat_draw_texture_region(disc, q, q, q, q, x + w - r, y + h - r, r, r, argb);
    plat_fill_rectf(x + r, y, w - 2 * r, h, argb);
    plat_fill_rectf(x, y + r, r, h - 2 * r, argb);
    plat_fill_rectf(x + w - r, y + r, r, h - 2 * r, argb);
}

void draw_rrect_outline(float x, float y, float w, float h, float r, float thickness,
                        uint32_t argb)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w * 0.5f)
        r = w * 0.5f;
    if (r > h * 0.5f)
        r = h * 0.5f;
    int ri = (int)(r + 0.5f), t = (int)(thickness + 0.5f);
    PlatTexture *ring = ri >= t ? ring_texture(ri, t) : NULL;
    if (!ring)
    {
        plat_fill_rectf(x, y, w, t, argb);
        plat_fill_rectf(x, y + h - t, w, t, argb);
        plat_fill_rectf(x, y + t, t, h - 2 * t, argb);
        plat_fill_rectf(x + w - t, y + t, t, h - 2 * t, argb);
        return;
    }
    plat_draw_texture_region(ring, 0, 0, ri, ri, x, y, ri, ri, argb);
    plat_draw_texture_region(ring, ri, 0, ri, ri, x + w - ri, y, ri, ri, argb);
    plat_draw_texture_region(ring, 0, ri, ri, ri, x, y + h - ri, ri, ri, argb);
    plat_draw_texture_region(ring, ri, ri, ri, ri, x + w - ri, y + h - ri, ri, ri, argb);
    plat_fill_rectf(x + ri, y, w - 2 * ri, t, argb);
    plat_fill_rectf(x + ri, y + h - t, w - 2 * ri, t, argb);
    plat_fill_rectf(x, y + ri, t, h - 2 * ri, argb);
    plat_fill_rectf(x + w - t, y + ri, t, h - 2 * ri, argb);
}

void draw_circle(float cx, float cy, float radius, uint32_t argb)
{
    PlatTexture *disc = disc_texture();
    if (disc)
        plat_draw_texture_region(disc, 0, 0, DISC, DISC, cx - radius, cy - radius, radius * 2,
                                 radius * 2, argb);
}

void draw_switch(float x, float y, float h, float t)
{
    float w = h * 2.1f;
    draw_rrect(x, y, w, h, h * 0.5f, argb_lerp(TH_SWITCH_OFF, TH_SWITCH_ON, t));
    draw_circle(x + h * 0.5f + (w - h) * t, y + h * 0.5f, h * 0.36f, 0xffffffffu);
}

float draw_choice(float right_x, float y, float h, float text_size, uint32_t fill, uint32_t text_argb,
                  const char *text)
{
    float icon = text_size * 1.1f, pad = h * 0.3f;
    float w = text_width(text_size, FONT_REGULAR, text) + 2 * icon + 2 * pad + 16;
    float x = right_x - w;
    draw_rrect(x, y, w, h, h * 0.5f, fill);
    icon_draw(ICON_CHEVRON_LEFT, x + pad, y + (h - icon) * 0.5f, icon, argb_alpha(text_argb, 0.6f));
    text_draw(x + pad + icon + 8, y + (h - text_size * 1.2f) * 0.5f, text_size, FONT_REGULAR, text_argb,
              ALIGN_LEFT, text);
    icon_draw(ICON_CHEVRON_RIGHT, right_x - pad - icon, y + (h - icon) * 0.5f, icon,
              argb_alpha(text_argb, 0.6f));
    return w;
}

float draw_pill(float x, float y, float h, float text_size, uint32_t fill, uint32_t text_argb,
                const char *text)
{
    float w = text_width(text_size, FONT_REGULAR, text) + h * 0.9f;
    draw_rrect(x, y, w, h, h * 0.5f, fill);
    text_draw(x + h * 0.45f, y + (h - text_size * 1.2f) * 0.5f, text_size, FONT_REGULAR, text_argb,
              ALIGN_LEFT, text);
    return w;
}

void draw_vgradient(float x, float y, float w, float h, const uint32_t *colors,
                    const float *stops, int count)
{
    PlatVertex v[6 * 8];
    int n = 0;
    for (int i = 0; i + 1 < count && i < 8; ++i)
    {
        float y0 = y + h * stops[i], y1 = y + h * stops[i + 1];
        quad(&v[n], x, y0, x + w, y0, x + w, y1, x, y1, colors[i], colors[i], colors[i + 1],
             colors[i + 1]);
        n += 6;
    }
    plat_draw_mesh(NULL, v, n, NULL, 0);
}

void draw_line(float x0, float y0, float x1, float y1, float thickness, uint32_t argb)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f)
        return;
    float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
    PlatVertex v[6];
    quad(v, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, argb, argb,
         argb, argb);
    plat_draw_mesh(NULL, v, 6, NULL, 0);
}

void draw_ring(float cx, float cy, float radius, float thickness, uint32_t argb)
{
    enum { SEGMENTS = 32 };
    PlatVertex v[SEGMENTS * 6];
    float r0 = radius - thickness * 0.5f, r1 = radius + thickness * 0.5f;
    for (int i = 0; i < SEGMENTS; ++i)
    {
        float a0 = 6.2831853f * i / SEGMENTS, a1 = 6.2831853f * (i + 1) / SEGMENTS;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        quad(&v[i * 6], cx + c0 * r1, cy + s0 * r1, cx + c1 * r1, cy + s1 * r1, cx + c1 * r0,
             cy + s1 * r0, cx + c0 * r0, cy + s0 * r0, argb, argb, argb, argb);
    }
    plat_draw_mesh(NULL, v, SEGMENTS * 6, NULL, 0);
}

void draw_glow(float x, float y, float w, float h, float spread, uint32_t argb)
{
    /* Four trapezoids around the rectangle, opaque on the inside edge. */
    uint32_t in = argb, out = argb & 0x00ffffffu;
    float X0 = x - spread, Y0 = y - spread, X1 = x + w + spread, Y1 = y + h + spread;
    PlatVertex v[24];
    quad(&v[0], X0, Y0, X1, Y0, x + w, y, x, y, out, out, in, in);             /* top */
    quad(&v[6], x + w, y, X1, Y0, X1, Y1, x + w, y + h, in, out, out, in);     /* right */
    quad(&v[12], x, y + h, x + w, y + h, X1, Y1, X0, Y1, in, in, out, out);    /* bottom */
    quad(&v[18], X0, Y0, x, y, x, y + h, X0, Y1, out, in, in, out);           /* left */
    plat_draw_mesh(NULL, v, 24, NULL, 0);
}

static const char *glyph_label(enum PadGlyph g)
{
    switch (g)
    {
    case GLYPH_L1: return "L1";
    case GLYPH_R1: return "R1";
    case GLYPH_L2: return "L2";
    case GLYPH_R2: return "R2";
    case GLYPH_L3: return "L3";
    case GLYPH_R3: return "R3";
    case GLYPH_START: return "OPTIONS";
    case GLYPH_SELECT: return "SELECT";
    case GLYPH_PS1_START: return "START";
    default: return NULL;
    }
}

float pad_glyph_width(enum PadGlyph glyph, float size)
{
    const char *label = glyph_label(glyph);
    if (label)
        return text_width(size * 0.6f, FONT_BOLD, label) + size * 0.7f;
    if (glyph == GLYPH_TOUCHPAD)
        return size * 1.8f;
    return size;
}

float draw_pad_glyph(enum PadGlyph glyph, float cx, float cy, float size)
{
    float r = size * 0.36f, t = size * 0.11f;
    float w = pad_glyph_width(glyph, size);
    const char *label = glyph_label(glyph);
    if (label)
    {
        /* shoulder buttons and OPTIONS/SELECT: a labelled chip */
        float h = size * 0.92f;
        draw_rrect(cx - w * 0.5f, cy - h * 0.5f, w, h, h * 0.3f, TH_ROW_SELECTED);
        text_draw(cx, cy - size * 0.6f * 0.62f, size * 0.6f, FONT_BOLD, TH_TEXT, ALIGN_CENTER,
                  label);
        return w;
    }
    switch (glyph)
    {
    case GLYPH_CROSS:
        draw_line(cx - r, cy - r, cx + r, cy + r, t, 0xff9fc0ffu);
        draw_line(cx + r, cy - r, cx - r, cy + r, t, 0xff9fc0ffu);
        break;
    case GLYPH_CIRCLE:
        draw_rrect_outline(cx - r - t * 0.5f, cy - r - t * 0.5f, 2 * r + t, 2 * r + t, r + t * 0.5f,
                           t, 0xffff8f8fu);
        break;
    case GLYPH_SQUARE:
        draw_rrect_outline(cx - r, cy - r, 2 * r, 2 * r, t, t, 0xfff3a0d0u);
        break;
    case GLYPH_TRIANGLE:
    {
        float top = cy - r * 1.05f, bottom = cy + r * 0.75f;
        draw_line(cx, top, cx + r, bottom, t, 0xff5fd3a8u);
        draw_line(cx + r, bottom, cx - r, bottom, t, 0xff5fd3a8u);
        draw_line(cx - r, bottom, cx, top, t, 0xff5fd3a8u);
        break;
    }
    case GLYPH_UP:
    case GLYPH_DOWN:
    case GLYPH_LEFT:
    case GLYPH_RIGHT:
    {
        static const int icons[] = {ICON_CHEVRON_UP, ICON_CHEVRON_DOWN, ICON_CHEVRON_LEFT,
                                    ICON_CHEVRON_RIGHT};
        float h = size * 0.92f;
        draw_rrect(cx - h * 0.5f, cy - h * 0.5f, h, h, h * 0.22f, TH_ROW_SELECTED);
        icon_draw(icons[glyph - GLYPH_UP], cx - h * 0.4f, cy - h * 0.4f, h * 0.8f, TH_TEXT);
        break;
    }
    case GLYPH_TOUCHPAD:
    {
        float h = size * 0.8f;
        draw_rrect_outline(cx - w * 0.5f, cy - h * 0.5f, w, h, h * 0.25f, size * 0.08f,
                           TH_TEXT_SOFT);
        break;
    }
    case GLYPH_NONE:
        draw_rect(cx - r, cy - t * 0.5f, 2 * r, t, TH_TEXT_DIM);
        break;
    default:
        break;
    }
    return w;
}

float hint_combo_width(const int *glyphs, int count, const char *label, float size)
{
    float w = 0;
    for (int i = 0; i < count; ++i)
        w += pad_glyph_width((enum PadGlyph)glyphs[i], size) + (i < count - 1 ? size * 0.9f : 0);
    if (label && *label)
        w += size * 0.35f + text_width(size, FONT_REGULAR, tr(label));
    return w;
}

float draw_hint_combo(float x, float y, const int *glyphs, int count, char sep, const char *label,
                      float size, uint32_t argb)
{
    float start = x;
    for (int i = 0; i < count; ++i)
    {
        float gw = pad_glyph_width((enum PadGlyph)glyphs[i], size);
        draw_pad_glyph((enum PadGlyph)glyphs[i], x + gw * 0.5f, y + size * 0.6f, size);
        x += gw;
        if (i < count - 1)
        {
            char mark[2] = {sep, '\0'};
            text_draw(x + size * 0.45f, y, size, FONT_REGULAR, argb, ALIGN_CENTER, mark);
            x += size * 0.9f;
        }
    }
    if (label && *label)
        text_draw(x + size * 0.35f, y, size, FONT_REGULAR, argb, ALIGN_LEFT, tr(label));
    return x - start + (label && *label ? size * 0.35f + text_width(size, FONT_REGULAR, tr(label)) : 0);
}

float draw_hint(float x, float y, enum PadGlyph glyph, const char *label, float size,
                uint32_t argb)
{
    float gw = pad_glyph_width(glyph, size);
    draw_pad_glyph(glyph, x + gw * 0.5f, y + size * 0.6f, size);
    label = tr(label);
    text_draw(x + gw + size * 0.35f, y, size, FONT_REGULAR, argb, ALIGN_LEFT, label);
    return gw + size * 0.35f + text_width(size, FONT_REGULAR, label) + size * 1.4f;
}
