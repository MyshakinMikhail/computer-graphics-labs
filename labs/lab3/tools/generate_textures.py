#!/usr/bin/env python3
"""Generate original, tileable PNG assets. Runtime reads these files with stb_image.

Python standard library only. Assets may be freely reused (CC0).
"""
import math
from pathlib import Path
import struct
import zlib

DESTINATION = Path(__file__).resolve().parents[1] / "textures"
SIZE = 256


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(name, pixel, size=SIZE):
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            rows.extend(max(0, min(255, round(c))) for c in pixel(x / size, y / size))
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    (DESTINATION / name).write_bytes(data)


def perlin(u, v, period=8):
    """Periodic gradient Perlin noise, including matching tile boundaries."""
    x, y = u * period, v * period
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy

    def dot(dx, dy):
        h = (((ix + dx) % period) * 1836311903 ^ ((iy + dy) % period) * 2971215073) & 0xFFFFFFFF
        angle = (h % 65536) * math.tau / 65536
        return math.cos(angle) * (fx - dx) + math.sin(angle) * (fy - dy)

    def fade(t):
        return t * t * t * (t * (t * 6 - 15) + 10)

    sx, sy = fade(fx), fade(fy)
    a, b = dot(0, 0), dot(1, 0)
    c, d = dot(0, 1), dot(1, 1)
    return (a + sx * (b - a)) * (1 - sy) + (c + sx * (d - c)) * sy


def noise(u, v):
    return perlin(u, v, 4) + 0.45 * perlin(u, v, 8) + 0.2 * perlin(u, v, 16)


def copper(u, v):
    x, y = (u * 4) % 1, (v * 4) % 1
    seam = min(x, 1 - x, y, 1 - y) < 0.026
    rivet = math.hypot(min(x, 1 - x) - 0.09, min(y, 1 - y) - 0.09) < 0.025
    return seam, rivet, 0.83 + 0.15 * noise(u, v) + 0.04 * math.sin(v * math.tau * 80)


def jade(u, v):
    x, y = (u * 4) % 1, (v * 4) % 1
    trace = (abs(x - 0.5) < 0.035 and y < 0.7) or (abs(y - 0.68) < 0.025 and x > 0.5)
    node = math.hypot(x - 0.5, y - 0.68) < 0.075
    glow = max(0.0, min(1.0, (noise(u, v) + 0.25) * 1.4)) if trace or node else 0
    return trace or node, glow


def lava(u, v):
    return max(0.0, 1 - abs(noise(u, v)) / 0.08) ** 1.5


def main():
    DESTINATION.mkdir(parents=True, exist_ok=True)
    png("black.png", lambda u, v: (0, 0, 0), 1)
    png("copper_albedo.png", lambda u, v: (18, 26, 30) if copper(u, v)[0] else
        (245, 208, 138) if copper(u, v)[1] else tuple(c * copper(u, v)[2] for c in (187, 100, 48)))
    png("copper_specular.png", lambda u, v: (8, 8, 8) if copper(u, v)[0] else
        (240, 240, 240) if copper(u, v)[1] else (155, 155, 155))
    png("jade_albedo.png", lambda u, v: (40, 145, 115) if jade(u, v)[0] else
        tuple(c * (0.85 + 0.4 * noise(u, v)) for c in (28, 58, 60)))
    png("jade_specular.png", lambda u, v: (225, 225, 225) if jade(u, v)[0] else (45, 45, 45))
    png("jade_emissive.png", lambda u, v: tuple(c * jade(u, v)[1] for c in (20, 255, 165)))
    png("lava_albedo.png", lambda u, v: (30 + 160 * lava(u, v), 26 + 55 * lava(u, v), 32))
    png("lava_specular.png", lambda u, v: tuple(15 + 70 * (1 - lava(u, v)) for _ in range(3)))
    png("lava_emissive.png", lambda u, v: tuple(c * lava(u, v) for c in (255, 75, 6)))
    png("checker_albedo.png", lambda u, v: (205, 215, 218) if
        (int(u * 8) + int(v * 8)) % 2 else (31, 83, 100))
    png("slate_albedo.png", lambda u, v: (18, 25, 32) if min(u, 1-u, v, 1-v) < 0.012 else
        tuple(c * (0.9 + 0.35 * noise(u, v)) for c in (75, 91, 108)))
    png("slate_specular.png", lambda u, v: (3, 3, 3) if min(u, 1-u, v, 1-v) < 0.012 else (65, 65, 65))
    print(f"Generated 12 PNG files in {DESTINATION}")


if __name__ == "__main__":
    main()
