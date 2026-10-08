#!/usr/bin/env python3
"""Generates src/core/swanstation_options.{h,c}: SwanStation's libretro core options as a C table, so the
settings menu can offer them and host.c can pass them to the core.

    python3 tools/gen_swanstation_options.py

Reads third_party/swanstation/src/libretro/libretro_core_options.h (the English definitions, with the
preprocessor evaluated for the PS5: x64, not Windows). The generated files are committed: building PSXS5
does not need to run this. Options PSXS5 already draws itself, or forces, are left out (SKIP below).
SPDX-License-Identifier: GPL-3.0-or-later
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "third_party/swanstation/src/libretro/libretro_core_options.h"
OUT_H = ROOT / "src/core/swanstation_options.h"
OUT_C = ROOT / "src/core/swanstation_options.c"

# Defined for this target when the header is read.
DEFINES = {"CPU_X64": True}

# Drawn by PSXS5's own settings (and applied in host.c apply_swanstation_options), or forced:
SKIP = {
    "swanstation_Console_Region",          # Settings > System > Region
    "swanstation_BIOS_PatchFastBoot",      # PS1 startup intro
    "swanstation_CDROM_ReadSpeedup",       # Fast CD loading
    "swanstation_GPU_ResolutionScale",     # Internal resolution
    "swanstation_GPU_TrueColor",           # True colour
    "swanstation_GPU_PGXPEnable",          # PGXP
    "swanstation_Main_RunaheadFrameCount", # Run-ahead (PSXS5 does it)
    "swanstation_GPU_Renderer",            # always Vulkan: the PS5 screen is drawn through it
    "swanstation_CPU_FastmemMode",         # always LUT: MMap does not survive a title's sandbox
}

CATEGORIES = [("console", "Console"), ("enhancement", "Enhancements"), ("display", "Display"),
              ("port", "Controller ports"), ("advanced", "Advanced")]


def preprocess(text):
    """Keeps the lines a PS5 build sees: #if/#ifdef/#ifndef/#else/#endif over DEFINES."""
    out, stack = [], []  # stack of (active, taken_branch_active_before)

    def cond(expr):
        expr = expr.strip()
        expr = re.sub(r"defined\((\w+)\)", lambda m: str(bool(DEFINES.get(m.group(1), False))), expr)
        expr = re.sub(r"\b(?!True\b|False\b)[A-Za-z_]\w*\b", lambda m: str(bool(DEFINES.get(m.group(0), False))), expr)
        expr = expr.replace("||", " or ").replace("&&", " and ").replace("!", " not ")
        return bool(eval(expr))

    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#"):
            m = re.match(r"#\s*(\w+)\s*(.*)", s)
            d, rest = m.group(1), m.group(2)
            parent = all(a for a, _ in stack)
            if d == "ifdef":
                stack.append((bool(DEFINES.get(rest.strip(), False)), None))
            elif d == "ifndef":
                stack.append((not DEFINES.get(rest.strip(), False), None))
            elif d == "if":
                stack.append((cond(rest), None))
            elif d == "else":
                a, _ = stack.pop()
                stack.append((not a, None))
            elif d == "endif":
                stack.pop()
            continue
        if all(a for a, _ in stack):
            out.append(line)
    return "\n".join(out)


def tokenize(text):
    """C tokens: strings (adjacent ones are joined), NULL, { } , ; comments dropped."""
    i, n = 0, len(text)
    toks = []
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
        elif text.startswith("//", i):
            i = text.index("\n", i)
        elif c == '"':
            j = i + 1
            buf = []
            while text[j] != '"':
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            s = "".join(buf)
            if toks and toks[-1][0] == "str":
                toks[-1] = ("str", toks[-1][1] + s)
            else:
                toks.append(("str", s))
            i = j + 1
        elif c in "{},":
            toks.append((c, c))
            i += 1
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            toks.append(("id", text[i:j]))
            i = j
        else:
            i += 1
    return toks


def parse(toks, pos=0):
    """Parses '{ ... }' starting at toks[pos] == '{' into a nested list."""
    assert toks[pos][0] == "{"
    pos += 1
    items = []
    while toks[pos][0] != "}":
        t = toks[pos]
        if t[0] == "{":
            sub, pos = parse(toks, pos)
            items.append(sub)
        elif t[0] == ",":
            pos += 1
        elif t[0] == "str":
            items.append(t[1])
            pos += 1
        elif t[0] == "id":
            items.append(None if t[1] == "NULL" else t[1])
            pos += 1
        else:
            pos += 1
    return items, pos + 1


def unescape(s):
    return s.replace('\\"', '"').replace("\\n", " ").replace("\\\\", "\\")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    text = preprocess(SRC.read_text())
    start = text.index("option_defs_us[]")
    start = text.index("{", text.index("=", start))
    toks = tokenize(text[start:])
    root, _ = parse(toks, 0)
    entries = [e for e in root if isinstance(e, list) and e and e[0]]
    opts = []
    for e in entries:
        key, label, _, desc, _, cat, pairs, default = e[:8]
        if key in SKIP or not key.startswith("swanstation_"):
            continue
        vals = [(p[0], p[1] or p[0]) for p in pairs if isinstance(p, list) and p and p[0] is not None]
        names = [v for v, _ in vals]
        if default not in names:
            sys.exit(f"{key}: default {default!r} is not one of its values")
        opts.append((key, unescape(label), unescape(desc or ""), cat, vals, names.index(default)))

    order = {c: i for i, (c, _) in enumerate(CATEGORIES)}
    opts.sort(key=lambda o: (order[o[3]],))  # stable: keeps the core's order inside a category

    h = ["/* Generated by tools/gen_swanstation_options.py - do not edit. SwanStation's core options. */",
         "#pragma once", "",
         f"#define SS_OPT_COUNT {len(opts)}", "",
         "enum SsCategory"]
    h.append("{")
    for c, _ in CATEGORIES:
        h.append(f"    SSC_{c.upper()},")
    h += ["    SSC_COUNT", "};", "",
          "typedef struct", "{",
          "    const char *key;   /* the libretro variable, swanstation_... */",
          "    const char *label; /* English; tr() leaves it as it is when there is no translation */",
          "    const char *help;",
          "    unsigned char category, count, def;",
          "    const char *const *values; /* what the core is given */",
          "    const char *const *labels; /* what the menu shows */",
          "} SsOpt;", "",
          "extern const SsOpt SS_OPTS[SS_OPT_COUNT];",
          "extern const char *const SS_CATEGORY_NAMES[SSC_COUNT];", ""]
    OUT_H.write_text("\n".join(h))

    c = ["/* Generated by tools/gen_swanstation_options.py - do not edit. */",
         '#include "swanstation_options.h"', ""]
    for i, (key, label, desc, cat, vals, d) in enumerate(opts):
        c.append(f"static const char *const v{i}[] = {{" + ", ".join(c_str(v) for v, _ in vals) + "};")
        c.append(f"static const char *const l{i}[] = {{" + ", ".join(c_str(unescape(l)) for _, l in vals) + "};")
    c += ["", "const char *const SS_CATEGORY_NAMES[SSC_COUNT] = {" + ", ".join(c_str(n) for _, n in CATEGORIES) + "};",
          "", "const SsOpt SS_OPTS[SS_OPT_COUNT] = {"]
    for i, (key, label, desc, cat, vals, d) in enumerate(opts):
        c.append(f"    {{{c_str(key)}, {c_str(label)},\n     {c_str(desc)},\n     SSC_{cat.upper()}, {len(vals)}, {d}, v{i}, l{i}}},")
    c += ["};", ""]
    OUT_C.write_text("\n".join(c))
    counts = {}
    for o in opts:
        counts[o[3]] = counts.get(o[3], 0) + 1
    print(len(opts), "options:", counts)


main()
