# SwanStationPS5 - reads the boot serial (SLUS-01041 ...) straight from a PS1 disc image.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every PS1 disc has SYSTEM.CNF in its root with a line like
    BOOT = cdrom:\\SLUS_010.41;1
which is the game's serial. It is region-exact, so covers can be matched by it.
Supports .cue/.bin (raw 2352-byte sectors), .iso/.img (2048), .pbp (PARAM.SFO)
and .m3u (first disc)."""
from __future__ import annotations

import re
import struct
from pathlib import Path

SERIAL_RE = re.compile(r"([A-Z]{4})[_-]?(\d{3})\.?(\d{2})")


def _format(raw: str) -> str | None:
    m = SERIAL_RE.search(raw.upper())
    return f"{m.group(1)}-{m.group(2)}{m.group(3)}" if m else None


class _Image:
    def __init__(self, path: Path):
        self.f = path.open("rb")
        size = path.stat().st_size
        self.sector, self.offset = 2048, 0
        for sector, offset in ((2352, 24), (2352, 16), (2048, 0)):
            if size >= 17 * sector:
                self.f.seek(16 * sector + offset)
                if self.f.read(6)[1:6] == b"CD001":
                    self.sector, self.offset = sector, offset
                    return
        raise ValueError("no ISO9660 volume")

    def read(self, lba: int, length: int) -> bytes:
        out = bytearray()
        while len(out) < length:
            self.f.seek(lba * self.sector + self.offset)
            out += self.f.read(min(2048, length - len(out)))
            lba += 1
        return bytes(out)

    def close(self):
        self.f.close()


def _from_iso(path: Path) -> str | None:
    img = _Image(path)
    try:
        pvd = img.read(16, 2048)
        root_lba, root_size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        root = img.read(root_lba, min(root_size, 65536))
        pos = 0
        while pos < len(root):
            n = root[pos]
            if n == 0:  # records never span sectors
                pos = (pos // 2048 + 1) * 2048
                continue
            name_len = root[pos + 32]
            name = root[pos + 33: pos + 33 + name_len].decode("ascii", "replace").upper()
            if name.startswith("SYSTEM.CNF"):
                lba = struct.unpack_from("<I", root, pos + 2)[0]
                size = struct.unpack_from("<I", root, pos + 10)[0]
                text = img.read(lba, min(size, 2048)).decode("ascii", "replace")
                boot = re.search(r"BOOT\s*=\s*\S*?([A-Z]{4}[_-]?\d{3}\.?\d{2})", text, re.I)
                return _format(boot.group(1)) if boot else None
            pos += n
    finally:
        img.close()
    return None


def _from_pbp(path: Path) -> str | None:
    with path.open("rb") as f:
        head = f.read(40)
        if head[:4] != b"\x00PBP":
            return None
        sfo_off, icon_off = struct.unpack_from("<II", head, 8)
        f.seek(sfo_off)
        sfo = f.read(icon_off - sfo_off)
    key_tab, data_tab, count = struct.unpack_from("<III", sfo, 8)
    for i in range(count):
        k_off, _, d_len, _, d_off = struct.unpack_from("<HHIII", sfo, 20 + i * 16)
        key = sfo[key_tab + k_off: sfo.index(b"\0", key_tab + k_off)].decode()
        if key == "DISC_ID":
            return _format(sfo[data_tab + d_off: data_tab + d_off + d_len].decode("ascii", "replace"))
    return None


def disc_serial(path: Path) -> str | None:
    """Serial of the game at `path`, or None when it can't be read."""
    path = Path(path)
    ext = path.suffix.lower()
    try:
        if ext == ".m3u":
            first = next((l.strip() for l in path.read_text(errors="replace").splitlines()
                          if l.strip() and not l.startswith("#")), None)
            return disc_serial(path.parent / first) if first else None
        if ext == ".cue":
            m = re.search(r'FILE\s+"([^"]+)"', path.read_text(errors="replace"), re.I)
            return disc_serial(path.parent / m.group(1)) if m else None
        if ext == ".pbp":
            return _from_pbp(path)
        if ext in {".bin", ".iso", ".img"}:
            return _from_iso(path)
    except (OSError, ValueError, struct.error, StopIteration):
        return None
    return None


if __name__ == "__main__":
    import sys
    for arg in sys.argv[1:]:
        print(disc_serial(Path(arg)) or "?", arg)
