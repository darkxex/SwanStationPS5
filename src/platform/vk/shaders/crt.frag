// SwanStationPS5 - a light CRT: scanlines that follow the PS1's own lines (whatever
// the internal resolution), an aperture-grille mask and a little bloom.
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Quad
{
    vec4 dst, uv, info, colour; // info: texture width, height, the PS1's line count
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
    vec3 c = texture(picture, in_uv).rgb;
    // where this pixel falls in its PS1 line: bright in the middle, dark at the gap
    float lines = max(quad.info.z, 1.0);
    float v = (in_uv.y - quad.uv.y) / max(quad.uv.w - quad.uv.y, 1e-5);
    float d = fract(v * lines) - 0.5;
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    // bright pixels bloom over the gap, as on a real tube
    float scan = mix(1.0 - 1.6 * d * d * 2.0, 1.0, luma * 0.45);
    // an RGB grille, every third column of the TV's pixels
    int m = int(mod(gl_FragCoord.x, 3.0));
    vec3 mask = m == 0 ? vec3(1.0, 0.82, 0.82) : m == 1 ? vec3(0.82, 1.0, 0.82) : vec3(0.82, 0.82, 1.0);
    c = c * scan * mask * 1.18;
    out_color = vec4(grade(min(c, vec3(1.0))), 1.0);
}
