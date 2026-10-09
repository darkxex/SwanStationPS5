#!/usr/bin/env python3
# SwanStationPS5 - compiles the four NTSC presets (third_party/ntsc: Themaister's NTSC shader from libretro's
# slang-shaders; 320px / 256px, S-Video / composite) to SPIR-V and writes
# src/platform/vk/shaders_ntsc.h: each preset's passes, in the same form as shaders_royale.h (RoyalePass), so the
# renderer in vk_present.c runs them the same way. Committed, so builds don't need a shader compiler:
#
#   GLSLANG=/path/to/glslang python3 tools/make-ntsc.py
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
base = os.path.join(root, "third_party", "ntsc")
shader_dir = os.path.join(base, "shaders", "maister")
out_path = os.path.join(root, "src", "platform", "vk", "shaders_ntsc.h")
glslang = os.environ.get("GLSLANG") or shutil.which("glslang") or shutil.which("glslangValidator")
if not glslang:
    sys.exit("glslang not found (set GLSLANG)")

# the order of Settings > Display > Shader, from 5 on
PRESETS = ["ntsc-320px-svideo", "ntsc-320px-composite", "ntsc-256px-svideo", "ntsc-256px-composite"]
STOCK = "shaders/maister/ntsc-stock.slang"


def read_preset(path):
    values = {}
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        m = re.match(r'(\w+)\s*=\s*"?([^"]*)"?\s*$', line)
        if m:
            values[m.group(1)] = m.group(2).strip()
    return values


def expand(text, directory):
    """The #include lines replaced by the files' text."""
    def one(m):
        return expand(open(os.path.join(directory, m.group(1)), encoding="utf-8").read(), directory)
    return re.sub(r'^#include "([^"]+)"[ \t]*$', one, text, flags=re.M)


def split_stages(text):
    parts = re.split(r"^#pragma stage (vertex|fragment)\s*$", text, flags=re.M)
    stages = {}
    for k in range(1, len(parts), 2):
        stages[parts[k]] = parts[k + 1]
    return parts[0], stages["vertex"], stages["fragment"]


def parse_parameters(text):
    params = {}
    for m in re.finditer(r"^#pragma parameter (\w+) \"[^\"]*\" (\S+) ", text, flags=re.M):
        params[m.group(1)] = float(m.group(2))
    return params


def push_only(common):
    """The shader's parameters all in one push constant block: a UBO with more than the MVP becomes the block (the
    instance keeps its name), and the MVP is the identity (the quad's corners are already in clip space)."""
    ubo = re.search(r"layout\(std140, set = 0, binding = 0\) uniform UBO\s*\{(.*?)\}\s*global;", common, flags=re.S)
    if ubo:
        members = [l.strip() for l in ubo.group(1).splitlines() if l.strip() and not l.strip().startswith("mat4 MVP")]
        if members:
            common = common.replace(ubo.group(0), "layout(push_constant) uniform Push\n{\n   " + "\n   ".join(members) + "\n} global;")
        else:
            common = common.replace(ubo.group(0), "")
    return re.sub(r"\b(global|registers)\.MVP\b", "mat4(1.0)", common)


TYPE_SIZE = {"vec4": (16, 16), "float": (4, 4), "uint": (4, 4), "vec2": (8, 8)}
PUSH_KINDS = {"SourceSize": "SOURCE_SIZE", "OriginalSize": "ORIGINAL_SIZE", "OutputSize": "OUTPUT_SIZE",
              "FinalViewportSize": "FINAL_SIZE", "FrameCount": "FRAME"}


def describe(name, common, frag, text):
    push = re.search(r"layout\(push_constant\)\s+uniform\s+Push\s*\{(.*?)\}\s*\w+;", common, flags=re.S)
    local = parse_parameters(text)
    members, offset = [], 0
    for line in (push.group(1).splitlines() if push else []):
        line = line.split("//", 1)[0].strip()
        m = re.match(r"(vec4|vec2|float|uint)\s+(\w+);", line)
        if not m:
            continue
        kind_t, member = m.groups()
        size, align = TYPE_SIZE[kind_t]
        offset = (offset + align - 1) // align * align
        if member in PUSH_KINDS:
            members.append((offset, PUSH_KINDS[member], -1, 0.0))
        elif member in local:
            members.append((offset, "PARAM", -1, local[member]))
        else:
            sys.exit("%s: push constant %s is not known" % (name, member))
        offset += size
    samplers = []
    for m in re.finditer(r"layout\(set = 0, binding = (\d+)\) uniform sampler2D (\w+);", frag):
        if m.group(2) != "Source":
            sys.exit("%s: sampler %s is not known" % (name, m.group(2)))
        samplers.append((int(m.group(1)), "SOURCE", -1))
    return (offset + 15) // 16 * 16, members, samplers


tmp = tempfile.mkdtemp()


def compile_stage(source, stage, name):
    path = os.path.join(tmp, name + "." + ("vert" if stage == "vertex" else "frag"))
    open(path, "w", encoding="utf-8").write(source)
    spv = path + ".spv"
    r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.1", "-o", spv, path], capture_output=True, text=True)
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


modules = {}  # shader file -> (id, vert words, frag words, push_size, members, samplers)


def module_of(rel):
    if rel in modules:
        return modules[rel]
    path = os.path.join(base, rel)
    text = expand(open(path, encoding="utf-8").read(), os.path.dirname(path))
    common, vertex, fragment = split_stages(text)
    common = push_only(common)
    vertex = re.sub(r"layout\(location = 0\) in vec4 Position;\s*layout\(location = 1\) in vec2 TexCoord;", "", vertex)
    vertex = ("#define TexCoord vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1))\n"
              "#define Position vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0)\n") + push_only(vertex)
    fragment = push_only(fragment)
    push_size, members, samplers = describe(rel, common, fragment, text)
    ident = "NTSC_M%d" % len(modules)
    entry = (ident, compile_stage(common + vertex, "vertex", ident), compile_stage(common + fragment, "fragment", ident),
             push_size, members, samplers)
    modules[rel] = entry
    print("%-52s push %3d bytes, %d members, %d samplers" % (rel, push_size, len(members), len(samplers)))
    return entry


SCALE = {"source": "SCALE_SOURCE", "viewport": "SCALE_VIEWPORT", "absolute": "SCALE_ABSOLUTE"}


def scale_of(preset, i, axis):
    typ = preset.get("scale_type_%s%d" % (axis, i)) or preset.get("scale_type%d" % i) or "source"
    val = preset.get("scale_%s%d" % (axis, i)) or preset.get("scale%d" % i) or "1.0"
    return SCALE[typ], float(val)


chains = []
for name in PRESETS:
    preset = read_preset(os.path.join(base, name + ".slangp"))
    count = int(preset["shaders"])
    passes = []
    for i in range(count):
        mod = module_of(preset["shader%d" % i])
        sx, vx = scale_of(preset, i, "x")
        sy, vy = scale_of(preset, i, "y")
        passes.append(dict(mod=mod, sx=sx, vx=vx, sy=sy, vy=vy,
                           linear=1 if preset.get("filter_linear%d" % i, "false") == "true" else 0,
                           fmt=2 if preset.get("float_framebuffer%d" % i, "false") == "true" else 0,
                           mod_frames=int(preset.get("frame_count_mod%d" % i, "0"))))
    if passes[-1]["sx"] != "SCALE_VIEWPORT" or passes[-1]["sy"] != "SCALE_VIEWPORT":
        # the chain ends smaller than the screen: one more pass stretches it, smoothly, as RetroArch does
        passes.append(dict(mod=module_of(STOCK), sx="SCALE_VIEWPORT", vx=1.0, sy="SCALE_VIEWPORT", vy=1.0, linear=1,
                           fmt=0, mod_frames=0))
    chains.append((name, passes))

out = ["/* Generated by tools/make-ntsc.py from third_party/ntsc (the NTSC shader by Themaister). Do not edit. */",
       "#ifndef SwanStationPS5_SHADERS_NTSC_H", "#define SwanStationPS5_SHADERS_NTSC_H", "#include <stdint.h>",
       '#include "shaders_royale.h"', ""]
for rel, (ident, vert, frag, *_rest) in modules.items():
    out.append("/* %s */" % rel)
    out.append(c_array(ident + "_VERT", vert))
    out.append(c_array(ident + "_FRAG", frag))
out.append("")
for n, (name, passes) in enumerate(chains):
    out.append("/* %s */" % name)
    out.append("static const RoyalePass NTSC_CHAIN%d[%d] = {" % (n, len(passes)))
    for p in passes:
        ident, vert, frag, push_size, members, samplers = p["mod"]
        out.append("    {%s_VERT, %s_FRAG, %d, %d, ROYALE_%s, ROYALE_%s, %.8ff, %.8ff, %d, %d, %d, %d, %d, %d," %
                   (ident, ident, len(vert) * 4, len(frag) * 4, p["sx"], p["sy"], p["vx"], p["vy"], p["linear"], 0,
                    p["fmt"], push_size, len(members), len(samplers)))
        out.append("     {%s}," % ", ".join("{%d, ROYALE_PUSH_%s, %d, %.8ff}" % m for m in members))
        out.append("     {%s}, %d}," % (", ".join("{%d, ROYALE_SAMPLER_%s, %d}" % s for s in samplers), p["mod_frames"]))
    out.append("};")
out.append("")
out.append("static const struct { const RoyalePass *passes; int count; } NTSC_PRESETS[%d] = {" % len(chains))
for n, (name, passes) in enumerate(chains):
    out.append("    {NTSC_CHAIN%d, %d}, /* %s */" % (n, len(passes), name))
out.append("};")
out.append("#endif")
open(out_path, "w").write("\n".join(out) + "\n")
shutil.rmtree(tmp)
print("wrote", out_path)
