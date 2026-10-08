#!/usr/bin/env python3
# PSXS5 - compiles the crt-royale-fast preset (third_party/crt-royale, RetroArch
# slang format) to SPIR-V and writes src/platform/vk/shaders_royale.h: each
# pass's vertex and fragment stage, what its push constants and samplers
# stand for, the shared parameter block and the three phosphor mask textures.
# Committed, so builds don't need a shader compiler. Run after changing the
# preset or a shader:
#
#   GLSLANG=/path/to/glslang python3 tools/make-royale.py
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
base = os.path.join(root, "third_party", "crt-royale")
src_dir = os.path.join(base, "src-fast")
out_path = os.path.join(root, "src", "platform", "vk", "shaders_royale.h")
glslang = os.environ.get("GLSLANG") or shutil.which("glslang") or shutil.which("glslangValidator")
if not glslang:
    sys.exit("glslang not found (set GLSLANG)")


# ---------------------------------------------------------------- the preset

def read_preset(path):
    values = {}
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        m = re.match(r'(\w+)\s*=\s*"?([^"]*)"?\s*$', line)
        if m:
            values[m.group(1)] = m.group(2).strip()
    return values


preset = read_preset(os.path.join(base, "crt-royale-fast.slangp"))
count = int(preset["shaders"])
aliases = {}
for i in range(count):
    a = preset.get("alias%d" % i, "")
    if a:
        aliases[a] = i
texture_names = [t for t in preset.get("textures", "").split(";") if t]


# ---------------------------------------------------------------- GLSL

def split_stages(text):
    """The common part, the vertex part and the fragment part of a .slang file."""
    parts = re.split(r"^#pragma stage (vertex|fragment)\s*$", text, flags=re.M)
    common = parts[0]
    stages = {}
    for k in range(1, len(parts), 2):
        stages[parts[k]] = parts[k + 1]
    return common, stages["vertex"], stages["fragment"]


def with_includes(text):
    # right after #version
    return re.sub(r"^(#version[^\n]*\n)", r"\1#extension GL_GOOGLE_include_directive : enable\n", text, count=1, flags=re.M)


def compile_stage(source, stage, tmp, name):
    path = os.path.join(tmp, name + "." + ("vert" if stage == "vertex" else "frag"))
    open(path, "w", encoding="utf-8").write(source)
    spv = path + ".spv"
    cmd = [glslang, "-V", "--target-env", "vulkan1.1", "-I" + src_dir, "-o", spv, path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("glslang failed on %s:\n%s%s" % (path, r.stdout, r.stderr))
    data = open(spv, "rb").read()
    return struct.unpack("<%dI" % (len(data) // 4), data)


def c_array(name, words):
    lines = ["static const uint32_t %s[] = {" % name]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


# ---------------------------------------------------------------- resources

PUSH_KINDS = {"SourceSize": ("SOURCE_SIZE", -1), "OriginalSize": ("ORIGINAL_SIZE", -1),
              "OutputSize": ("OUTPUT_SIZE", -1), "FinalViewportSize": ("FINAL_SIZE", -1),
              "FrameCount": ("FRAME", -1)}
TYPE_SIZE = {"vec4": (16, 16), "float": (4, 4), "uint": (4, 4), "vec2": (8, 8)}


def parse_parameters(text):
    params = {}
    for m in re.finditer(r"^#pragma parameter (\w+) \"[^\"]*\" (\S+) ", text, flags=re.M):
        params[m.group(1)] = float(m.group(2))
    return params


bind_text = open(os.path.join(src_dir, "bind-shader-params.h"), encoding="utf-8").read()
ubo_match = re.search(r"layout\(std140, set = 0, binding = 0\) uniform UBO\s*\{(.*?)\}\s*global;", bind_text, flags=re.S)
ubo_members = []
for line in ubo_match.group(1).splitlines():
    line = line.split("//", 1)[0].strip()
    m = re.match(r"(mat4|float)\s+(\w+);", line)
    if m:
        ubo_members.append((m.group(1), m.group(2)))
ubo_defaults = parse_parameters(bind_text)


def pass_description(i, common, frag, text):
    """Push constant members and sampler bindings of a pass."""
    push = re.search(r"layout\(push_constant\)\s+uniform\s+Push\s*\{(.*?)\}\s*params;", common, flags=re.S)
    local_params = parse_parameters(text)
    members, offset = [], 0
    for line in push.group(1).splitlines():
        line = line.split("//", 1)[0].strip()
        m = re.match(r"(vec4|vec2|float|uint)\s+(\w+);", line)
        if not m:
            continue
        kind_t, name = m.groups()
        size, align = TYPE_SIZE[kind_t]
        offset = (offset + align - 1) // align * align
        if name in PUSH_KINDS:
            kind, index = PUSH_KINDS[name]
            default = 0.0
        elif name.endswith("Size") and name[:-4] in aliases:
            kind, index, default = "PASS_SIZE", aliases[name[:-4]], 0.0
        elif name.endswith("Size") and name[:-4] in texture_names:
            kind, index, default = "TEXTURE_SIZE", texture_names.index(name[:-4]), 0.0
        elif name in local_params:
            kind, index, default = "PARAM", -1, local_params[name]
        elif name.endswith("Size"):
            kind, index, default = "NONE", -1, 0.0  # an alias the preset does not define
        else:
            sys.exit("pass %d: push constant %s is not known" % (i, name))
        members.append((offset, kind, index, default))
        offset += size
    push_size = (offset + 15) // 16 * 16
    samplers = []
    for m in re.finditer(r"layout\(set = 0, binding = (\d+)\) uniform sampler2D (\w+);", frag):
        binding, name = int(m.group(1)), m.group(2)
        if name == "Source":
            kind, index = "SOURCE", -1
        elif name == "Original":
            kind, index = "ORIGINAL", -1
        elif name in aliases:
            kind, index = "PASS", aliases[name]
        elif name in texture_names:
            kind, index = "TEXTURE", texture_names.index(name)
        else:
            sys.exit("pass %d: sampler %s is not known" % (i, name))
        samplers.append((binding, kind, index))
    return push_size, members, samplers


SCALE = {"source": "SCALE_SOURCE", "viewport": "SCALE_VIEWPORT", "absolute": "SCALE_ABSOLUTE"}


def scale_of(i, axis):
    typ = preset.get("scale_type_%s%d" % (axis, i)) or preset.get("scale_type%d" % i) or "source"
    val = preset.get("scale_%s%d" % (axis, i)) or preset.get("scale%d" % i) or "1.0"
    return SCALE[typ], float(val)


# ---------------------------------------------------------------- PNG masks

def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            assert depth == 8 and color in (2, 6) and interlace == 0, "unsupported PNG"
        elif kind == b"IDAT":
            idat += chunk
    channels = 4 if color == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        rows.append(bytes(line))
        prev = line
    rgba = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            rgba += px if channels == 4 else px + b"\xff"
    return width, height, bytes(rgba)


# ---------------------------------------------------------------- build

out = ["/* Generated by tools/make-royale.py from third_party/crt-royale (crt-royale-fast). Do not edit. */",
       "#ifndef PSXS5_SHADERS_ROYALE_H", "#define PSXS5_SHADERS_ROYALE_H", "#include <stdint.h>", "",
       "enum { ROYALE_SCALE_SOURCE, ROYALE_SCALE_VIEWPORT, ROYALE_SCALE_ABSOLUTE };",
       "enum { ROYALE_PUSH_NONE, ROYALE_PUSH_SOURCE_SIZE, ROYALE_PUSH_ORIGINAL_SIZE, ROYALE_PUSH_OUTPUT_SIZE,",
       "       ROYALE_PUSH_FINAL_SIZE, ROYALE_PUSH_FRAME, ROYALE_PUSH_PASS_SIZE, ROYALE_PUSH_TEXTURE_SIZE,",
       "       ROYALE_PUSH_PARAM };",
       "enum { ROYALE_SAMPLER_SOURCE, ROYALE_SAMPLER_ORIGINAL, ROYALE_SAMPLER_PASS, ROYALE_SAMPLER_TEXTURE };",
       "enum { ROYALE_WRAP_BORDER, ROYALE_WRAP_EDGE, ROYALE_WRAP_REPEAT };",
       "typedef struct { uint16_t offset; uint8_t kind; int8_t index; float value; } RoyalePush;",
       "typedef struct { uint8_t binding, kind; int8_t index; } RoyaleSampler;",
       "typedef struct",
       "{",
       "    const uint32_t *vert, *frag;",
       "    uint32_t vert_size, frag_size;",
       "    uint8_t scale_x_type, scale_y_type;",
       "    float scale_x, scale_y;",
       "    uint8_t linear, wrap, srgb;",
       "    uint16_t push_size;",
       "    uint8_t push_count, sampler_count;",
       "    RoyalePush push[12];",
       "    RoyaleSampler samplers[6];",
       "} RoyalePass;",
       "typedef struct { uint16_t width, height; uint8_t linear, wrap; const uint8_t *rgba; } RoyaleTexture;", ""]

tmp = tempfile.mkdtemp()
pass_defs = []
for i in range(count):
    shader = os.path.basename(preset["shader%d" % i])
    text = open(os.path.join(src_dir, shader), encoding="utf-8").read()
    common, vertex, fragment = split_stages(text)
    # no vertex buffer: the quad's corners come from the vertex index
    vertex = re.sub(r"layout\(location = 0\) in vec4 Position;\s*layout\(location = 1\) in vec2 TexCoord;", "", vertex)
    vertex = ("#define TexCoord vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1))\n"
              "#define Position vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0)\n") + vertex
    push_size, members, samplers = pass_description(i, common, fragment, text)
    vert_spv = compile_stage(with_includes(common) + vertex, "vertex", tmp, "pass%d" % i)
    frag_spv = compile_stage(with_includes(common) + fragment, "fragment", tmp, "pass%d" % i)
    out.append("/* pass %d: %s */" % (i, shader))
    out.append(c_array("ROYALE_P%d_VERT" % i, vert_spv))
    out.append(c_array("ROYALE_P%d_FRAG" % i, frag_spv))
    sx, vx = scale_of(i, "x")
    sy, vy = scale_of(i, "y")
    linear = 1 if preset.get("filter_linear%d" % i, "false") == "true" else 0
    wrap = {"clamp_to_border": 0, "clamp_to_edge": 1, "repeat": 2}[preset.get("wrap_mode%d" % i, "clamp_to_border")]
    srgb = 1 if preset.get("srgb_framebuffer%d" % i, "false") == "true" else 0
    pass_defs.append((i, push_size, members, samplers, sx, vx, sy, vy, linear, wrap, srgb, len(vert_spv), len(frag_spv)))
    print("pass %d %-62s push %3d bytes, %d members, %d samplers" % (i, shader, push_size, len(members), len(samplers)))

out.append("")
out.append("static const RoyalePass ROYALE_PASSES[%d] = {" % count)
for (i, push_size, members, samplers, sx, vx, sy, vy, linear, wrap, srgb, vn, fn) in pass_defs:
    out.append("    {ROYALE_P%d_VERT, ROYALE_P%d_FRAG, %d, %d, ROYALE_%s, ROYALE_%s, %.8ff, %.8ff, %d, %d, %d, %d, %d, %d," %
               (i, i, vn * 4, fn * 4, sx, sy, vx, vy, linear, wrap, srgb, push_size, len(members), len(samplers)))
    out.append("     {%s}," % ", ".join("{%d, ROYALE_PUSH_%s, %d, %.8ff}" % m for m in members))
    out.append("     {%s}}," % ", ".join("{%d, ROYALE_SAMPLER_%s, %d}" % s for s in samplers))
out.append("};")

# the shared parameter block (std140): MVP, then one float each
ubo = []
for kind, name in ubo_members:
    if kind == "mat4":
        ubo += [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
    else:
        if name not in ubo_defaults:
            sys.exit("parameter %s has no default" % name)
        ubo.append(ubo_defaults[name])
out.append("")
out.append("/* UBO: an identity MVP, then the parameters' defaults (std140) */")
out.append("static const float ROYALE_UBO[%d] = {%s};" % (len(ubo), ", ".join("%.8ff" % v for v in ubo)))
out.append("/* index in ROYALE_UBO of each parameter, for the settings */")
for n, (kind, name) in enumerate(ubo_members):
    pass
idx = 0
for kind, name in ubo_members:
    if kind == "float":
        out.append("#define ROYALE_UBO_%s %d" % (name.upper(), idx))
        idx += 1
    else:
        idx += 16

# the phosphor masks
out.append("")
tex_defs = []
for n, name in enumerate(texture_names):
    path = os.path.join(base, "masks", os.path.basename(preset[name]))
    w, h, rgba = read_png(path)
    out.append("static const uint8_t ROYALE_TEX%d[%d] = {" % (n, len(rgba)))
    for k in range(0, len(rgba), 32):
        out.append("    " + ",".join(str(b) for b in rgba[k:k + 32]) + ",")
    out.append("};")
    linear = 1 if preset.get(name + "_linear", "true") == "true" else 0
    wrap = {"clamp_to_border": 0, "clamp_to_edge": 1, "repeat": 2}[preset.get(name + "_wrap_mode", "clamp_to_border")]
    tex_defs.append("    {%d, %d, %d, %d, ROYALE_TEX%d}," % (w, h, linear, wrap, n))
out.append("static const RoyaleTexture ROYALE_TEXTURES[%d] = {" % len(texture_names))
out += tex_defs
out.append("};")
out.append("#endif")
open(out_path, "w").write("\n".join(out) + "\n")
shutil.rmtree(tmp)
print("wrote", out_path)
