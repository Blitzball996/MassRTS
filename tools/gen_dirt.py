#!/usr/bin/env python3
"""Generate a seamless, natural-looking dirt/soil texture.
The shipped dirt.png was a world/earth map by mistake (blue oceans), which made
terrain show blue ripples. This builds a tileable brown soil texture instead."""
import math, random, struct, zlib, sys

W = H = 256
random.seed(20240601)

# --- value-noise helpers (tileable via wrap) ---
def make_lattice(n):
    return [[random.random() for _ in range(n)] for _ in range(n)]

def smooth(t):
    return t * t * (3 - 2 * t)

def sample(lat, n, u, v):
    # u,v in [0,1); lattice wraps for seamless tiling
    x = u * n; y = v * n
    x0 = int(x) % n; y0 = int(y) % n
    x1 = (x0 + 1) % n; y1 = (y0 + 1) % n
    fx = smooth(x - int(x)); fy = smooth(y - int(y))
    a = lat[y0][x0] * (1 - fx) + lat[y0][x1] * fx
    b = lat[y1][x0] * (1 - fx) + lat[y1][x1] * fx
    return a * (1 - fy) + b * fy

# fractal noise: several octaves of tileable value noise
octaves = [(4, 1.0), (8, 0.5), (16, 0.25), (32, 0.13), (64, 0.07)]
lats = {n: make_lattice(n) for n, _ in octaves}

def fbm(u, v):
    s = 0.0; amp = 0.0
    for n, a in octaves:
        s += sample(lats[n], n, u, v) * a
        amp += a
    return s / amp

# base earthy palette (dark loam -> mid brown -> light dry soil)
dark  = (62, 44, 30)
mid   = (104, 76, 50)
light = (150, 116, 78)

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

pixels = bytearray()
for y in range(H):
    pixels.append(0)  # PNG filter byte (none) per scanline
    for x in range(W):
        u = x / W; v = y / H
        n = fbm(u, v)                       # 0..1 large-scale variation
        # fine grain speckle
        grain = (sample(lats[64], 64, u, v) - 0.5) * 0.35
        t = max(0.0, min(1.0, n + grain))
        if t < 0.5:
            col = lerp(dark, mid, t / 0.5)
        else:
            col = lerp(mid, light, (t - 0.5) / 0.5)
        # scattered small pebbles / darker clumps
        peb = sample(lats[32], 32, u * 2 % 1, v * 2 % 1)
        if peb > 0.82:
            col = lerp(col, (90, 88, 84), (peb - 0.82) / 0.18 * 0.6)
        pixels.extend(col)

def png_chunk(typ, data):
    c = typ + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

out = b"\x89PNG\r\n\x1a\n"
out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))  # RGB
out += png_chunk(b"IDAT", zlib.compress(bytes(pixels), 9))
out += png_chunk(b"IEND", b"")

dst = sys.argv[1] if len(sys.argv) > 1 else "assets/textures/blocks/dirt.png"
with open(dst, "wb") as f:
    f.write(out)
print("wrote", dst, len(out), "bytes")
