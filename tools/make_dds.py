#!/usr/bin/env python3
# PSXS5 - converts the home-screen backgrounds to the BC7 DDS files the PS5 shell reads.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Converts the two 3840x2160 backgrounds to pic0.dds / pic1.dds:

    python3 tools/make_dds.py
        sce_sys/background-source.png        -> sce_sys/pic0.dds  (selected on the home screen)
        sce_sys/launch-background-source.png -> sce_sys/pic1.dds  (while the app starts)
    python3 tools/make_dds.py SRC.png OUT.dds [SRC.png OUT.dds ...]
    python3 tools/make_dds.py --check           # also decode each file and print its PSNR

The output is what tools/validate-assets.sh asks for: one 3840x2160 BC7_UNORM texture in a DX10
DDS, no mipmaps. It needs Pillow and numpy, not texconv. The encoder writes BC7 mode 6 (one
colour line per 4x4 block, 16 steps), which is plenty for these smooth gradients.
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SIZE = (3840, 2160)
PAIRS = [("sce_sys/launch-background-source.png", "sce_sys/pic0.dds"),
         ("sce_sys/launch-background-source.png", "sce_sys/pic1.dds")]
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.float32) / 64.0
CHUNK = 32768  # blocks per step, to keep memory small


def header(width, height):
    """DDS + DX10 header, byte for byte what texconv writes for BC7_UNORM without mipmaps."""
    linear = width // 4 * (height // 4) * 16
    dds = struct.pack("<4sI", b"DDS ", 124)
    dds += struct.pack("<IIIIII", 0x000A1007, height, width, linear, 1, 1)  # flags, h, w, size, depth, mips
    dds += bytes(44)                                                          # reserved
    dds += struct.pack("<II4sIIIII", 32, 4, b"DX10", 0, 0, 0, 0, 0)         # pixel format: DX10
    dds += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)                          # caps
    dx10 = struct.pack("<IIIII", 98, 3, 0, 1, 3)                              # BC7_UNORM, 2D, 1, opaque
    return dds + dx10


def blocks_of(img):
    a = np.asarray(img.convert("RGB"), dtype=np.float32)
    h, w, _ = a.shape
    return a.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)


def quantize(c):
    """c: (n, 3) wanted colour of one endpoint -> (7-bit values (n,3), p-bit (n,), 8-bit colour (n,3)).
    The shared p-bit becomes the low bit of the three 8-bit channels."""
    best_err = None
    for p in (1,):  # always 1: alpha is 127*2+p, so 255 (opaque); colours lose at most one level
        q = np.clip(np.rint((c - p) / 2.0), 0, 127)
        full = q * 2 + p
        err = ((full - c) ** 2).sum(axis=1)
        if best_err is None:
            best_err, best_q, best_p, best_full = err, q, np.full(len(c), p), full
        else:
            m = err < best_err
            best_err = np.where(m, err, best_err)
            best_q = np.where(m[:, None], q, best_q)
            best_p = np.where(m, p, best_p)
            best_full = np.where(m[:, None], full, best_full)
    return best_q.astype(np.uint64), best_p.astype(np.uint64), best_full.astype(np.float32)


def encode_chunk(px):
    n = len(px)
    mean = px.mean(axis=1, keepdims=True)
    d = px - mean
    cov = np.einsum("npc,npd->ncd", d, d)
    _, vec = np.linalg.eigh(cov)
    axis = vec[:, :, -1]                                    # principal direction (n,3)
    t = np.einsum("npc,nc->np", d, axis)
    lo = mean[:, 0] + axis * t.min(axis=1)[:, None]
    hi = mean[:, 0] + axis * t.max(axis=1)[:, None]
    for _ in range(2):                                      # least-squares refit of the two ends
        q0, p0, e0 = quantize(np.clip(lo, 0, 255))
        q1, p1, e1 = quantize(np.clip(hi, 0, 255))
        line = e1 - e0
        denom = (line ** 2).sum(axis=1)
        s = np.einsum("npc,nc->np", px - e0[:, None], line) / np.maximum(denom, 1e-6)[:, None]
        s = np.clip(s, 0, 1)
        idx = np.abs(s[:, :, None] - WEIGHTS[None, None, :]).argmin(axis=2)
        w = WEIGHTS[idx]                                    # (n,16)
        a = (1 - w)
        # solve for the two endpoint colours that best fit pixel = a*E0 + w*E1
        saa, sab, sbb = (a * a).sum(1), (a * w).sum(1), (w * w).sum(1)
        det = saa * sbb - sab * sab
        ok = det > 1e-6
        ra = np.einsum("np,npc->nc", a, px)
        rb = np.einsum("np,npc->nc", w, px)
        det_s = np.where(ok, det, 1.0)
        n0 = (sbb[:, None] * ra - sab[:, None] * rb) / det_s[:, None]
        n1 = (saa[:, None] * rb - sab[:, None] * ra) / det_s[:, None]
        lo = np.where(ok[:, None], n0, lo)
        hi = np.where(ok[:, None], n1, hi)
    q0, p0, e0 = quantize(np.clip(lo, 0, 255))
    q1, p1, e1 = quantize(np.clip(hi, 0, 255))
    pal = e0[:, None, :] * (1 - WEIGHTS)[None, :, None] + e1[:, None, :] * WEIGHTS[None, :, None]
    idx = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(axis=3).argmin(axis=2)  # (n,16)
    swap = idx[:, 0] >= 8                                   # pixel 0's top index bit is implied 0
    idx = np.where(swap[:, None], 15 - idx, idx)
    q0, q1 = np.where(swap[:, None], q1, q0), np.where(swap[:, None], q0, q1)
    p0, p1 = np.where(swap, p1, p0), np.where(swap, p0, p1)
    idx = idx.astype(np.uint64)

    low = np.full(n, 1 << 6, np.uint64)                     # mode 6: six zero bits, then a one
    shift = 7
    for ch in range(3):                                     # R0 R1 G0 G1 B0 B1
        low |= q0[:, ch] << np.uint64(shift)
        low |= q1[:, ch] << np.uint64(shift + 7)
        shift += 14
    low |= np.uint64(127) << np.uint64(49)                  # A0 = A1 = 127, with the p-bits below: 255
    low |= np.uint64(127) << np.uint64(56)
    low |= p0 << np.uint64(63)
    high = p1.copy()
    high |= (idx[:, 0] & np.uint64(7)) << np.uint64(1)
    for i in range(1, 16):
        high |= idx[:, i] << np.uint64(4 + 4 * (i - 1))
    return low, high


def encode(img):
    """Image (3840x2160 RGB) -> the BC7 block bytes."""
    px = blocks_of(img)
    out = np.empty((len(px), 2), np.uint64)
    for start in range(0, len(px), CHUNK):
        low, high = encode_chunk(px[start:start + CHUNK])
        out[start:start + CHUNK, 0] = low
        out[start:start + CHUNK, 1] = high
    return out.astype("<u8").tobytes()


def decode(data, width, height):
    """BC7 mode 6 blocks -> RGB array, to check the encoder against."""
    b = np.frombuffer(data, "<u8").reshape(-1, 2)
    low, high = b[:, 0], b[:, 1]
    assert ((low & np.uint64(0x7F)) == 64).all(), "not mode 6"

    def field(shift):
        return ((low >> np.uint64(shift)) & np.uint64(127)).astype(np.float32)

    p0 = ((low >> np.uint64(63)) & np.uint64(1)).astype(np.float32)
    p1 = (high & np.uint64(1)).astype(np.float32)
    e0 = np.stack([field(7 + 14 * c) * 2 + p0 for c in range(3)], axis=1)
    e1 = np.stack([field(14 + 14 * c) * 2 + p1 for c in range(3)], axis=1)
    idx = np.empty((len(b), 16), np.int64)
    idx[:, 0] = ((high >> np.uint64(1)) & np.uint64(7)).astype(np.int64)
    for i in range(1, 16):
        idx[:, i] = ((high >> np.uint64(4 + 4 * (i - 1))) & np.uint64(15)).astype(np.int64)
    w = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.float32)[idx] / 64.0
    px = e0[:, None, :] * (1 - w)[:, :, None] + e1[:, None, :] * w[:, :, None]
    return px.reshape(height // 4, width // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(height, width, 3)


def convert(src, dst, check):
    img = Image.open(src).convert("RGB")
    if img.size != SIZE:
        sys.exit(f"error: {src} is {img.size[0]}x{img.size[1]}, the PS5 wants {SIZE[0]}x{SIZE[1]}")
    data = encode(img)
    dst.write_bytes(header(*SIZE) + data)
    print(f"wrote {dst} ({dst.stat().st_size} bytes)")
    if check:
        ref = np.asarray(img, dtype=np.float32)
        mse = ((decode(data, *SIZE) - ref) ** 2).mean()
        print(f"  PSNR {10 * np.log10(255 ** 2 / max(mse, 1e-9)):.1f} dB")


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    check = "--check" in sys.argv
    if len(args) % 2:
        sys.exit("usage: make_dds.py [--check] [SRC.png OUT.dds ...]")
    pairs = list(zip(args[0::2], args[1::2])) or [(ROOT / s, ROOT / d) for s, d in PAIRS]
    for src, dst in pairs:
        convert(Path(src), Path(dst), check)


main()
