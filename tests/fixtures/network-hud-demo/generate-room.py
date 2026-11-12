"""Generate assets/room.glb: an original procedural "weather and news observatory" interior.

Room coordinates are metres: x across, z depth (-z is the back wall), y up from the floor. The host keeps
the room's own walls, cards and walkway; this model only dresses the corners, ceiling and floor inlay, so it
stays inside x/z +-9.3 and below 5.8 m and leaves the centre aisle and wall consoles clear.
Full-height host columns belong only in the corners; the side-wall console bays remain open.
"""
import json
import math
import struct
import zlib
from pathlib import Path

assets = Path(__file__).parent / "assets"
assets.mkdir(exist_ok=True)

# 16 colours in 4x4-pixel blocks of a 16x16 PNG; UVs sample block centres so filtering never bleeds.
PALETTE = [
    (24, 30, 46), (52, 66, 92), (210, 216, 228), (120, 200, 255),   # steel, slate, paper, sky
    (255, 196, 64), (255, 120, 40), (80, 160, 255), (40, 230, 220),  # sun, flame, rain, cyan glow
    (236, 240, 248), (150, 156, 170), (180, 60, 220), (60, 200, 110),  # cloud, cloud shade, violet, leaf
    (200, 40, 50), (180, 140, 70), (16, 18, 24), (255, 240, 180),    # news red, brass, ink, warm light
]
STEEL, SLATE, PAPER, SKY, SUN, FLAME, RAIN, CYAN, CLOUD, SHADE, VIOLET, LEAF, RED, BRASS, INK, WARM = range(16)

rows = []
for y in range(16):
    row = bytearray(b"\x00")
    for x in range(16):
        row += bytes(PALETTE[(y // 4) * 4 + x // 4]) + b"\xff"
    rows.append(bytes(row))


def chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))

vertices = []  # (x, y, z, nx, ny, nz, u, v)
indices = []


def uv(colour):
    return ((colour % 4) * 4 + 2) / 16.0, ((colour // 4) * 4 + 2) / 16.0


def quad(corners, normal, colour):
    base = len(vertices)
    u, v = uv(colour)
    for c in corners:
        vertices.append((*c, *normal, u, v))
    indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))


def box(cx, cy, cz, sx, sy, sz, colour, yaw=0.0):
    """Axis box centred at (cx, cy, cz), optionally turned about y."""
    c, s = math.cos(yaw), math.sin(yaw)

    def p(x, y, z):
        return (cx + x * c + z * s, cy + y, cz - x * s + z * c)

    def n(x, y, z):
        return (x * c + z * s, y, -x * s + z * c)

    hx, hy, hz = sx / 2, sy / 2, sz / 2
    quad([p(-hx, -hy, hz), p(hx, -hy, hz), p(hx, hy, hz), p(-hx, hy, hz)], n(0, 0, 1), colour)
    quad([p(hx, -hy, -hz), p(-hx, -hy, -hz), p(-hx, hy, -hz), p(hx, hy, -hz)], n(0, 0, -1), colour)
    quad([p(hx, -hy, hz), p(hx, -hy, -hz), p(hx, hy, -hz), p(hx, hy, hz)], n(1, 0, 0), colour)
    quad([p(-hx, -hy, -hz), p(-hx, -hy, hz), p(-hx, hy, hz), p(-hx, hy, -hz)], n(-1, 0, 0), colour)
    quad([p(-hx, hy, hz), p(hx, hy, hz), p(hx, hy, -hz), p(-hx, hy, -hz)], n(0, 1, 0), colour)
    quad([p(-hx, -hy, -hz), p(hx, -hy, -hz), p(hx, -hy, hz), p(-hx, -hy, hz)], n(0, -1, 0), colour)


def sphere(cx, cy, cz, r, colour, rings=8, segments=12, squash=1.0, band=None):
    """Low-poly UV sphere; `band` optionally recolours the equator rings (a globe's ocean/land stripe)."""
    base = len(vertices)
    for i in range(rings + 1):
        phi = math.pi * i / rings
        tint = band if band is not None and abs(i - rings / 2) <= 1 else colour
        u, v = uv(tint)
        for j in range(segments + 1):
            theta = 2 * math.pi * j / segments
            nx, ny, nz = math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta)
            vertices.append((cx + r * nx, cy + r * ny * squash, cz + r * nz, nx, ny, nz, u, v))
    for i in range(rings):
        for j in range(segments):
            a = base + i * (segments + 1) + j
            b = a + segments + 1
            indices.extend((a, a + 1, b, a + 1, b + 1, b))


def ring(cx, cy, cz, radius, width, colour, tilt=0.0, segments=32):
    """Flat torus-like band (two-sided) for orbits and the floor compass."""
    for side in (1, -1):
        base = len(vertices)
        u, v = uv(colour)
        ct, st = math.cos(tilt), math.sin(tilt)
        for j in range(segments + 1):
            t = 2 * math.pi * j / segments
            for rr in (radius - width / 2, radius + width / 2):
                x, z = rr * math.cos(t), rr * math.sin(t)
                y, z = -z * st, z * ct
                vertices.append((cx + x, cy + y, cz + z, 0, side * ct, side * st, u, v))
        for j in range(segments):
            a = base + j * 2
            tri = (a, a + 1, a + 3, a, a + 3, a + 2) if side > 0 else (a, a + 3, a + 1, a, a + 2, a + 3)
            indices.extend(tri)


def cylinder(cx, cy, cz, r, h, colour, segments=12):
    base = len(vertices)
    u, v = uv(colour)
    for j in range(segments + 1):
        t = 2 * math.pi * j / segments
        nx, nz = math.cos(t), math.sin(t)
        vertices.append((cx + r * nx, cy, cz + r * nz, nx, 0, nz, u, v))
        vertices.append((cx + r * nx, cy + h, cz + r * nz, nx, 0, nz, u, v))
    for j in range(segments):
        a = base + j * 2
        indices.extend((a, a + 1, a + 3, a, a + 3, a + 2))
    top = len(vertices)
    vertices.append((cx, cy + h, cz, 0, 1, 0, u, v))
    for j in range(segments + 1):
        t = 2 * math.pi * j / segments
        vertices.append((cx + r * math.cos(t), cy + h, cz + r * math.sin(t), 0, 1, 0, u, v))
    for j in range(segments):
        indices.extend((top, top + j + 2, top + j + 1))


def cloud(cx, cy, cz, scale):
    for dx, dy, dz, r in ((0, 0, 0, 0.62), (0.6, -0.08, 0.1, 0.46), (-0.58, -0.1, -0.05, 0.44),
                          (0.2, 0.22, -0.2, 0.42), (-0.25, 0.12, 0.3, 0.38)):
        sphere(cx + dx * scale, cy + dy * scale, cz + dz * scale, r * scale, CLOUD if dy >= 0 else SHADE,
               rings=6, segments=10, squash=0.72)


# Floor inlay: a compass rose at the centre of the aisle (flush, so it reads through the reflective floor).
ring(0, 0.01, 1.2, 1.9, 0.12, BRASS, segments=40)
ring(0, 0.012, 1.2, 1.35, 0.06, CYAN, segments=40)
for k in range(8):
    t = k * math.pi / 4
    length = 1.8 if k % 2 == 0 else 1.1
    # The arms overlap at the hub: layer them so the north pointer cannot z-fight the other arms.
    y = 0.022 if k == 0 else (0.018 if k % 2 == 0 else 0.014)
    tip = (math.sin(t) * length, y, 1.2 - math.cos(t) * length)
    side = (math.cos(t) * 0.16, 0, math.sin(t) * 0.16)
    centre = (0, y, 1.2)
    colour = RED if k == 0 else (PAPER if k % 2 == 0 else SLATE)
    quad([(centre[0] - side[0], y, centre[2] - side[2]), (centre[0] + side[0], y, centre[2] + side[2]),
          tip, tip], (0, 1, 0), colour)

# Overhead centrepiece: a holographic globe in two orbit rings, hung from the ceiling on a brass stem.
sphere(0, 4.35, 1.2, 0.72, SKY, rings=10, segments=16, band=LEAF)
ring(0, 4.35, 1.2, 1.05, 0.05, CYAN, tilt=0.35)
ring(0, 4.35, 1.2, 1.25, 0.04, VIOLET, tilt=-0.6)
cylinder(0, 5.05, 1.2, 0.05, 0.75, BRASS, segments=8)

# Back-left: a weather mast with a spinning-cup anemometer, wind vane and rain gauge.
cylinder(-7.6, 0, -7.6, 0.55, 0.25, STEEL)
cylinder(-7.6, 0.25, -7.6, 0.09, 4.4, SLATE, segments=8)
for k in range(3):
    t = k * 2 * math.pi / 3
    ax, az = -7.6 + math.cos(t) * 0.55, -7.6 + math.sin(t) * 0.55
    box((-7.6 + ax) / 2, 4.55, (-7.6 + az) / 2, 0.55, 0.04, 0.04, BRASS, yaw=-t)
    sphere(ax, 4.55, az, 0.14, PAPER, rings=4, segments=8)
box(-7.6, 3.7, -7.6, 1.4, 0.05, 0.05, BRASS)
quad([(-6.9, 3.6, -7.6), (-6.5, 3.7, -7.6), (-6.9, 3.8, -7.6), (-6.9, 3.8, -7.6)], (0, 0, 1), RED)
cylinder(-6.3, 0, -8.3, 0.22, 1.1, SKY, segments=10)
cylinder(-6.3, 1.1, -8.3, 0.26, 0.06, STEEL, segments=10)

# Back-right: a radio telescope dish that pulls in the news wire.
cylinder(7.5, 0, -7.6, 0.6, 0.3, STEEL)
box(7.5, 1.4, -7.6, 0.3, 2.2, 0.3, SLATE)
dish_base = len(vertices)
u, v = uv(PAPER)
for i in range(7):
    rr = 1.5 * i / 6
    for j in range(17):
        t = 2 * math.pi * j / 16
        x, y = rr * math.cos(t), rr * math.sin(t)
        depth = 0.32 * (rr / 1.5) ** 2
        # The dish faces the room centre: tilt its axis up and towards +x/-z inverse (local z -> world).
        wx, wy, wz = 7.5 - 0.7 * (depth) - x * 0.5, 3.3 + y * 0.9 + depth * 0.4, -7.6 + x * 0.85 + depth * 0.7
        vertices.append((wx, wy, wz, -0.6, 0.4, 0.7, u, v))
for i in range(6):
    for j in range(16):
        a = dish_base + i * 17 + j
        b = a + 17
        indices.extend((a, b, a + 1, a + 1, b, b + 1, a, a + 1, b, a + 1, b + 1, b))
box(6.85, 3.6, -7.0, 0.06, 0.06, 1.2, BRASS, yaw=0.8)
sphere(6.4, 3.75, -6.6, 0.12, FLAME, rings=4, segments=8)

# Front corners: newspaper stacks beside a printing desk, and a reading lamp.
for side in (-1, 1):
    x = side * 7.4
    box(x, 0.45, 7.6, 2.4, 0.9, 1.0, STEEL)
    box(x, 0.93, 7.6, 2.5, 0.06, 1.1, BRASS)
    for k in range(6):
        box(x + side * 1.2, 0.1 + k * 0.16, 6.4 - (k % 2) * 0.05, 0.7, 0.14, 0.5, PAPER if k % 2 else WARM,
            yaw=0.12 * (k % 3 - 1))
    box(x - side * 0.5, 1.0, 7.5, 0.6, 0.04, 0.45, PAPER, yaw=0.2 * side)
    box(x - side * 0.5, 1.03, 7.4, 0.45, 0.01, 0.06, INK, yaw=0.2 * side)
    box(x + side * 0.6, 1.0, 7.7, 0.5, 0.04, 0.35, RED, yaw=-0.15 * side)
    cylinder(x + side * 0.9, 0.96, 7.9, 0.08, 0.9, BRASS, segments=8)
    sphere(x + side * 0.9, 1.95, 7.9, 0.2, WARM, rings=5, segments=8)

# Weather sculptures hanging below the ceiling: sun, rain cloud, storm cloud, snow cloud.
sphere(-4.6, 4.7, -4.4, 0.55, SUN, rings=8, segments=12)
for k in range(10):
    t = k * 2 * math.pi / 10
    box(-4.6 + math.cos(t) * 0.9, 4.7 + math.sin(t) * 0.9, -4.4, 0.42, 0.08, 0.08, FLAME, yaw=0)
cloud(4.4, 4.9, -4.2, 1.0)
for k in range(9):
    box(3.7 + (k % 3) * 0.55, 3.9 - (k // 3) * 0.45, -4.4 + (k % 2) * 0.3, 0.05, 0.28, 0.05, RAIN)
cloud(-4.6, 4.95, 4.6, 0.9)
for k, (x0, y0, x1, y1) in enumerate(((-4.5, 4.4, -4.8, 3.9), (-4.8, 3.9, -4.4, 3.8), (-4.4, 3.8, -4.7, 3.2))):
    box((x0 + x1) / 2, (y0 + y1) / 2, 4.6, 0.09, math.hypot(x1 - x0, y1 - y0), 0.09, SUN,
        yaw=0)
cloud(4.6, 5.0, 4.6, 0.85)
for k in range(6):
    sphere(4.0 + (k % 3) * 0.6, 3.9 - (k // 3) * 0.5, 4.6 + (k % 2) * 0.25, 0.07, PAPER, rings=3, segments=6)

# Ceiling news ribbons: red and paper bands crossing above the aisle like a newsroom headline rail.
for k in range(5):
    box(0, 5.55, -6 + k * 3.0, 7.0, 0.08, 0.22, RED if k % 2 == 0 else PAPER)

binary = bytearray()
for vtx in vertices:
    binary += struct.pack("<8f", *vtx)
vertex_bytes = len(binary)
for i in indices:
    binary += struct.pack("<I", i)
index_bytes = len(binary) - vertex_bytes
image_offset = len(binary)
binary += png
while len(binary) % 4:
    binary.append(0)

xs = [v[0] for v in vertices]; ys = [v[1] for v in vertices]; zs = [v[2] for v in vertices]
assert max(map(abs, xs)) <= 9.3 and max(map(abs, zs)) <= 9.3 and min(ys) >= 0 and max(ys) <= 5.8, (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs))
assert len(vertices) < 65536

manifest = {
    "asset": {"version": "2.0", "generator": "SciFi Shell network HUD demo room 1.1.2 - floor compass"},
    "buffers": [{"byteLength": len(binary)}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": vertex_bytes, "byteStride": 32},
        {"buffer": 0, "byteOffset": vertex_bytes, "byteLength": index_bytes},
        {"buffer": 0, "byteOffset": image_offset, "byteLength": len(png)},
    ],
    "accessors": [
        {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": len(vertices), "type": "VEC3",
         "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]},
        {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": len(vertices), "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": len(vertices), "type": "VEC2"},
        {"bufferView": 1, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
    ],
    "images": [{"bufferView": 2, "mimeType": "image/png"}],
    "samplers": [{"magFilter": 9728, "minFilter": 9728}],
    "textures": [{"source": 0, "sampler": 0}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0}}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3,
                                "material": 0}]}],
    "nodes": [{"mesh": 0}],
    "scenes": [{"nodes": [0]}],
    "scene": 0,
}
text = json.dumps(manifest, separators=(",", ":")).encode("ascii")
text += b" " * (-len(text) % 4)
glb = (struct.pack("<III", 0x46546C67, 2, 28 + len(text) + len(binary))
       + struct.pack("<II", len(text), 0x4E4F534A) + text
       + struct.pack("<II", len(binary), 0x004E4942) + binary)
(assets / "room.glb").write_bytes(glb)
print(f"room.glb: {len(vertices)} vertices, {len(indices) // 3} triangles, {len(glb)} bytes")
