"""Gym tables keyed by world tiles: traversal edges (TRAV) and area flags (AFLG), little-endian."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

TRAV_MAGIC = int.from_bytes(b"TRAV", "little")
AFLG_MAGIC = int.from_bytes(b"AFLG", "little")
TRAVERSAL_OBJECT = 1

AREA_MULTICOMBAT = 0x01
AREA_WILDERNESS = 0x02
AREA_WILDERNESS_LEVEL = 0x04  # row value = wilderness level inside the polygon

_EDGE = struct.Struct("<BBHIIHHHH4x")


@dataclass(frozen=True)
class Edge:
    """One traversal: using `option` of loc `source_id` from `start` moves the player to `dest`."""
    kind: int
    option: int
    source_id: int
    start: tuple[int, int, int]  # approach tile x, y, plane
    dest: tuple[int, int, int]
    flags: int
    action: str
    target: str


@dataclass(frozen=True)
class Zone:
    plane: int
    flags: int
    rect: tuple[int, int, int, int]  # world tiles x0, y0, x1, y1, inclusive
    value: int = 0


def read_traversal(path: Path) -> list[Edge]:
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<III", data, 0)
    if magic != TRAV_MAGIC or version != 1:
        raise SystemExit(f"{path}: not a TRAV v1 table")
    edges, pos = [], 12
    for _ in range(count):
        kind, option, planes, src, flags, sx, sy, dx, dy = _EDGE.unpack_from(data, pos)
        pos += _EDGE.size
        strings = []
        for _ in range(2):
            n = data[pos]
            strings.append(data[pos + 1:pos + 1 + n].decode("latin-1"))
            pos += 1 + n
        edges.append(Edge(kind, option, src, (sx, sy, planes >> 8), (dx, dy, planes & 0xFF), flags, *strings))
    return edges


def write_traversal(path: Path, edges: list[Edge]) -> None:
    if not edges:
        raise SystemExit("the engine needs at least one traversal edge")
    out = bytearray(struct.pack("<III", TRAV_MAGIC, 1, len(edges)))
    for e in sorted(edges, key=lambda e: (e.kind, e.source_id, e.start, e.option)):  # rows of one source stay contiguous
        out += _EDGE.pack(e.kind, e.option, e.start[2] << 8 | e.dest[2], e.source_id, e.flags,
                          e.start[0], e.start[1], e.dest[0], e.dest[1])
        for s in (e.action, e.target):
            raw = s.encode("latin-1")[:255]
            out += bytes([len(raw)]) + raw
    path.write_bytes(bytes(out))


def write_area_flags(path: Path, zones: list[Zone]) -> None:
    rows, points = bytearray(), 0
    for z in zones:
        x0, y0, x1, y1 = z.rect
        rows += struct.pack("<BBHIIHHHHHH", z.plane, 0, 4, z.flags, 0, x0, y0, x1, y1, z.value, 0)
        rows += struct.pack("<8H", x0, y0, x1, y0, x1, y1, x0, y1)
        points += 4
    path.write_bytes(struct.pack("<4I", AFLG_MAGIC, 1, len(zones), points) + bytes(rows))
