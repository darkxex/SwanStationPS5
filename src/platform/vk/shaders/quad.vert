// SwanStationPS5 v2 - a textured rectangle: the game picture, or the interface on top.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled into shaders_spv.h by tools/make-shaders.sh.
#version 450

layout(push_constant) uniform Quad
{
    vec4 dst;  // x0, y0, x1, y1 in clip space (-1..1, y down)
    vec4 uv;   // u0, v0, u1, v1
    vec4 info; // for the game's shaders (see sharp.frag, crt.frag)
    vec4 colour; // brightness, saturation, warmth (quad.frag)
} quad;

layout(location = 0) out vec2 out_uv;

void main()
{
    // two triangles from six vertex indices: 0 1 2, 2 1 3
    const int corner[6] = int[](0, 1, 2, 2, 1, 3);
    int c = corner[gl_VertexIndex];
    vec2 t = vec2(c & 1, c >> 1);
    gl_Position = vec4(mix(quad.dst.xy, quad.dst.zw, t), 0.0, 1.0);
    out_uv = mix(quad.uv.xy, quad.uv.zw, t);
}
