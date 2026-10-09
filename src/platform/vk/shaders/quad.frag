// SwanStationPS5 v2 - samples the rectangle's picture.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled into shaders_spv.h by tools/make-shaders.sh.
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Quad
{
    vec4 dst, uv, info, colour;
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

// Contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT): a cross of
// taps one screen pixel apart; flat areas and strong edges are left alone.
vec3 sharpen(vec2 uv, vec3 c, float amount)
{
    vec2 px = 1.0 / max(quad.info.xy * quad.info.zw, vec2(1.0)); // one screen pixel, in uv
    vec3 n = texture(picture, uv - vec2(0.0, px.y)).rgb;
    vec3 s = texture(picture, uv + vec2(0.0, px.y)).rgb;
    vec3 e = texture(picture, uv + vec2(px.x, 0.0)).rgb;
    vec3 w = texture(picture, uv - vec2(px.x, 0.0)).rgb;
    vec3 mn = min(c, min(min(n, s), min(e, w)));
    vec3 mx = max(c, max(max(n, s), max(e, w)));
    vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 weight = amp * (-1.0 / mix(8.0, 5.0, amount));
    return clamp((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight), 0.0, 1.0);
}

void main()
{
    vec4 c = texture(picture, in_uv);
    vec3 rgb = quad.colour.w > 0.0 ? sharpen(in_uv, c.rgb, quad.colour.w) : c.rgb;
    out_color = vec4(grade(rgb), c.a); // the interface passes 1, 1, 0, 0
}
