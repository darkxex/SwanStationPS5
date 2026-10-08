// PSXS5 - LCD3x: a handheld LCD look. Dark gaps between the PS1's lines and
// an RGB sub-pixel stripe along each of its columns.
// Ported from Gigaherz's lcd3x (RetroArch slang-shaders, public domain).
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Quad
{
    vec4 dst, uv, info, colour; // info: texture width, height, the PS1's line count
} quad;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// the original's parameters (Brighten Scanlines, Brighten LCD)
const float brighten_scanlines = 16.0;
const float brighten_lcd = 4.0;
const float PI = 3.141592654;
const vec3 offsets = vec3(PI) * vec3(1.0 / 2.0, 1.0 / 2.0 - 2.0 / 3.0, 1.0 / 2.0 - 4.0 / 3.0);

// the Display settings' brightness, saturation and warmth (1, 1, 0: unchanged)
vec3 grade(vec3 c)
{
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, quad.colour.y) * quad.colour.x;
    return clamp(c * vec3(1.0 + quad.colour.z, 1.0, 1.0 - quad.colour.z), 0.0, 1.0);
}

void main()
{
    vec3 res = texture(picture, in_uv).rgb;
    // the PS1's own grid, whatever the internal resolution: its lines, and
    // columns at 4:3 (320 for 240 lines, 640 for 480)
    float lines = max(quad.info.z, 1.0);
    float v = (in_uv.y - quad.uv.y) / max(quad.uv.w - quad.uv.y, 1e-5);
    vec2 angle = vec2(in_uv.x * lines * (4.0 / 3.0), v * lines) * (2.0 * PI);
    float yfactor = (brighten_scanlines + sin(angle.y)) / (brighten_scanlines + 1.0);
    vec3 xfactors = (brighten_lcd + sin(angle.x + offsets)) / (brighten_lcd + 1.0);
    out_color = vec4(grade(yfactor * xfactors * res), 1.0);
}
