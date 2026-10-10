// SwanStationPS5 - the interface drawn by the GPU: a textured, tinted triangle list in canvas pixels.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled into shaders_ui_spv.h by tools/make-ui-shaders.sh.
#version 450

layout(push_constant) uniform Screen
{
    vec2 size; // the canvas, in pixels
} screen;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_colour;

void main()
{
    gl_Position = vec4(in_pos / screen.size * 2.0 - 1.0, 0.0, 1.0);
    out_uv = in_uv;
    out_colour = in_colour;
}
