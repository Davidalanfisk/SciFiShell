"""Generate the small original model and texture used by the car example."""
import json
import struct
import zlib
from pathlib import Path

assets = Path(__file__).parent / "assets"
assets.mkdir(exist_ok=True)


def chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


rows = [
    b"\x00" + bytes([220, 40, 35, 255, 255, 170, 45, 255]) * 2,
    b"\x00" + bytes([255, 170, 45, 255, 220, 40, 35, 255]) * 2,
] * 2
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    + chunk(b"IEND", b"")
)
(assets / "paint.png").write_bytes(png)

# Each face has separate vertices so its normals and UVs remain independent.
faces = [
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
    ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
    ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
    ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
]
vertices = bytearray()
indices = bytearray()
for face, (normal, corners) in enumerate(faces):
    for corner, (u, v) in zip(corners, [(0, 0), (1, 0), (1, 1), (0, 1)]):
        vertices += struct.pack("<8f", *(c * 0.5 for c in corner), *normal, u, v)
    indices += struct.pack("<6H", *(face * 4 + n for n in (0, 1, 2, 0, 2, 3)))
binary = vertices + indices
while len(binary) % 4:
    binary.append(0)
manifest = {
    "asset": {"version": "2.0", "generator": "SciFi Shell car example"},
    "buffers": [{"byteLength": len(binary)}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(vertices), "byteStride": 32},
        {"buffer": 0, "byteOffset": len(vertices), "byteLength": len(indices)},
    ],
    "accessors": [
        {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 24, "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 24, "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": 24, "type": "VEC2"},
        {"bufferView": 1, "componentType": 5123, "count": 36, "type": "SCALAR"},
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
    "nodes": [{"mesh": 0}],
    "scenes": [{"nodes": [0]}],
    "scene": 0,
}
text = json.dumps(manifest, separators=(",", ":")).encode("ascii")
text += b" " * (-len(text) % 4)
glb = (
    struct.pack("<III", 0x46546C67, 2, 28 + len(text) + len(binary))
    + struct.pack("<II", len(text), 0x4E4F534A) + text
    + struct.pack("<II", len(binary), 0x004E4942) + binary
)
(assets / "car.glb").write_bytes(glb)
