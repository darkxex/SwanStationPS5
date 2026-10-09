// SwanStationPS5 - supersampling: a picture rendered bigger than the screen (4x, 8x,
// 16x internal resolution) averaged down to each screen pixel, instead of
// bilinear filtering, which skips texels and shimmers when things move.
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Quad
{
    vec4 dst, uv, info, colour; // info: texture width, height, then screen pixels per texel (x, y)
} quad;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// the Display settings' brightness, saturation and warmth (1, 1, 0: unchanged)
vec3 grade(vec3 c)
{
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, quad.colour.y) * quad.colour.x;
    return clamp(c * vec3(1.0 + quad.colour.z, 1.0, 1.0 - quad.colour.z), 0.0, 1.0);
}

void main()
{
    vec2 size = quad.info.xy;
    vec2 per = 1.0 / max(quad.info.zw, vec2(1e-3)); // texels under one screen pixel
    vec2 n = clamp(ceil(per * 0.5), vec2(1.0), vec2(4.0)); // bilinear taps, each covering 2x2 texels
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
        {
            if (i >= int(n.x) || j >= int(n.y))
                continue;
            vec2 o = ((vec2(i, j) + 0.5) / n - 0.5) * per / size;
            sum += texture(picture, in_uv + o).rgb;
        }
    out_color = vec4(grade(sum / (n.x * n.y)), 1.0);
}
