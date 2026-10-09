// SwanStationPS5 - sharp bilinear: each source pixel stays a crisp block, and only the
// seams between blocks are blended, so uneven scaling doesn't shimmer.
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
    vec2 scale = max(quad.info.zw, vec2(1.0)); // screen pixels per texel
    vec2 texel = in_uv * size;
    vec2 base = floor(texel);
    vec2 offset = fract(texel) - 0.5;
    vec2 region = 0.5 - 0.5 / scale;
    vec2 f = (offset - clamp(offset, -region, region)) * scale + 0.5;
    out_color = vec4(grade(texture(picture, (base + f) / size).rgb), 1.0);
}
