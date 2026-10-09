#!/usr/bin/env python3
# SwanStationPS5 - fails the build if eboot.bin imports a system module that stops
# a native title from launching ("Can't start the game or app").
# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys

# Found on hardware:
# - libSceKeyboard, libSceImeDialog (PacBrew SDL2): the title wouldn't launch
# - libScePosixForWebKit (only exporter of strcasestr): crashed when a game started
FORBIDDEN = {"libSceKeyboard", "libSceImeDialog", "libScePosixForWebKit"}

data = open(sys.argv[1], "rb").read()
modules = {m.decode() for m in re.findall(rb"(libSce[A-Za-z0-9_]+|libkernel[A-Za-z0-9_]*)(?=\x00)", data)}
print("imported modules:", ", ".join(sorted(modules)))
bad = sorted(modules & FORBIDDEN)
if bad:
    sys.exit(f"error: eboot.bin imports {', '.join(bad)}; the PS5 refuses to launch it. "
             "Provide the functions in src/platform/ps5_shims.c.")
