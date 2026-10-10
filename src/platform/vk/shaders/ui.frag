// SwanStationPS5 - the interface drawn by the GPU: the texture times the vertex colour.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled into shaders_ui_spv.h by tools/make-ui-shaders.sh.
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_colour;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(picture, in_uv) * in_colour;
}
