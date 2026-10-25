"""Read and write b237 mapsquare files: terrain (file 0) and locs (file 1)."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from rc_cache.locations import iter_location_placements

SIZE = 64
PLANES = 4
PROCEDURAL = -1  # height comes from the client's noise function at the tile's world position


@dataclass
class Tile:
    height: int = PROCEDURAL  # explicit height byte relative to the plane below, or PROCEDURAL
    underlay: int = 0
    overlay: int = 0
    shape: int = 0
    rotation: int = 0
    settings: int = 0


@dataclass(frozen=True)
class Loc:
    id: int
    plane: int
    x: int  # mapsquare-local in files, world tiles inside the compiler
    y: int
    type: int
    rotation: int


def empty_tiles() -> list:
    return [[[Tile() for _ in range(SIZE)] for _ in range(SIZE)] for _ in range(PLANES)]


def decode_terrain(data: bytes) -> list:
    """Decode terrain into tiles[plane][x][y], keeping procedural-height markers."""
    tiles = empty_tiles()
    pos = 0
    for plane in range(PLANES):
        for x in range(SIZE):
            for y in range(SIZE):
                tile = tiles[plane][x][y]
                while True:
                    if pos + 2 > len(data):
                        raise ValueError("truncated terrain data")
                    attr = (data[pos] << 8) | data[pos + 1]
                    pos += 2
                    if attr == 0:
                        break
                    if attr == 1:
                        tile.height = 0 if data[pos] == 1 else data[pos]
                        pos += 1
                        break
                    if attr <= 49:
                        tile.overlay = (data[pos] << 8) | data[pos + 1]
                        pos += 2
                        tile.shape = (attr - 2) // 4
                        tile.rotation = (attr - 2) & 3
                    elif attr <= 81:
                        tile.settings = attr - 49
                    else:
                        tile.underlay = attr - 81
    return tiles


def encode_terrain(tiles: list) -> bytes:
    out = bytearray()
    for plane in range(PLANES):
        for x in range(SIZE):
            for y in range(SIZE):
                t = tiles[plane][x][y]
                if not (0 <= t.shape <= 11 and 0 <= t.rotation <= 3 and 0 <= t.settings <= 32
                        and 0 <= t.underlay <= 0xFFFF - 81 and 0 <= t.overlay <= 0xFFFF):
                    raise ValueError(f"tile {plane},{x},{y} out of range: {t}")
                if t.overlay or t.shape or t.rotation:
                    out += struct.pack(">HH", 2 + t.shape * 4 + t.rotation, t.overlay)
                if t.settings:
                    out += struct.pack(">H", 49 + t.settings)
                if t.underlay:
                    out += struct.pack(">H", 81 + t.underlay)
                if t.height == PROCEDURAL:
                    out += b"\x00\x00"
                elif 0 <= t.height <= 255:
                    out += struct.pack(">HB", 1, t.height)
                else:
                    raise ValueError(f"tile {plane},{x},{y} height out of range: {t.height}")
    return bytes(out)


def decode_locs(data: bytes) -> list[Loc]:
    """Decode locs with mapsquare-local coordinates."""
    return [Loc(p.object_id, p.plane, p.local_x, p.local_y, p.shape, p.rotation)
            for p in iter_location_placements(data, 0, 0)]


def _write_smart(out: bytearray, value: int) -> None:
    if not 0 <= value < 32768:
        raise ValueError(f"smart out of range: {value}")
    if value < 128:
        out.append(value)
    else:
        out += struct.pack(">H", value + 32768)


def encode_locs(locs: list[Loc]) -> bytes:
    """Encode mapsquare-local locs: id deltas (chained smarts), then sorted packed positions."""
    out = bytearray()
    prev_id = -1
    current = None
    prev_pos = 0
    for loc in sorted(set(locs), key=lambda l: (l.id, l.plane << 12 | l.x << 6 | l.y, l.type, l.rotation)):
        if not (0 <= loc.x < SIZE and 0 <= loc.y < SIZE and 0 <= loc.plane < PLANES
                and 0 <= loc.type <= 22 and 0 <= loc.rotation <= 3):
            raise ValueError(f"loc out of range: {loc}")
        if loc.id != current:
            if current is not None:
                _write_smart(out, 0)
            delta = loc.id - prev_id
            while delta >= 32767:
                _write_smart(out, 32767)
                delta -= 32767
            _write_smart(out, delta)
            prev_id = current = loc.id
            prev_pos = 0
        packed = loc.plane << 12 | loc.x << 6 | loc.y
        _write_smart(out, packed - prev_pos + 1)
        prev_pos = packed
        out.append(loc.type << 2 | loc.rotation)
    if current is not None:
        _write_smart(out, 0)
    _write_smart(out, 0)
    return bytes(out)
