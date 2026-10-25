"""Read and validate mapsquare-indexed collision tile binaries."""
from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path
import struct


COLLISION_TILE_INDEX_MAGIC = 0x49505443  # CTPI
COLLISION_TILE_INDEX_VERSION = 1
COLLISION_TILE_MAPSQUARE_COUNT = 65536
COLLISION_TILE_MAX_PAGE_ROWS = 4 * 64 * 64

HEADER = struct.Struct("<9I")
INDEX_ENTRY = struct.Struct("<II")
RECORD = struct.Struct("<BBBBI")


def read_header(path: Path) -> tuple[int, ...]:
    with path.open("rb") as handle:
        raw = handle.read(HEADER.size)
    if len(raw) != HEADER.size:
        raise ValueError("short collision index header")
    header = HEADER.unpack(raw)
    if header[0] != COLLISION_TILE_INDEX_MAGIC:
        raise ValueError("wrong collision index magic")
    if header[1] != COLLISION_TILE_INDEX_VERSION:
        raise ValueError("unsupported collision index version")
    if header[3] != RECORD.size:
        raise ValueError("wrong collision record size")
    if header[4] > COLLISION_TILE_MAPSQUARE_COUNT:
        raise ValueError("invalid occupied collision page count")
    if sum(header[5:9]) != header[2]:
        raise ValueError("collision plane counts do not match row count")
    expected_size = (
        HEADER.size
        + COLLISION_TILE_MAPSQUARE_COUNT * INDEX_ENTRY.size
        + header[2] * RECORD.size
    )
    if path.stat().st_size != expected_size:
        raise ValueError("collision index has trailing or missing bytes")
    return header


def iter_collision_tiles(path: Path) -> Iterator[tuple[int, int, int, int, int]]:
    header = read_header(path)
    with path.open("rb") as handle:
        handle.seek(HEADER.size)
        index: list[tuple[int, int]] = []
        expected_first = 0
        occupied_pages = 0
        for _ in range(COLLISION_TILE_MAPSQUARE_COUNT):
            raw = handle.read(INDEX_ENTRY.size)
            if len(raw) != INDEX_ENTRY.size:
                raise ValueError("short collision page directory")
            first, count = INDEX_ENTRY.unpack(raw)
            if first != expected_first or count > header[2] - expected_first:
                raise ValueError("invalid collision page range")
            if count > COLLISION_TILE_MAX_PAGE_ROWS:
                raise ValueError("collision page exceeds tile capacity")
            index.append((first, count))
            expected_first += count
            occupied_pages += count > 0
        if expected_first != header[2] or occupied_pages != header[4]:
            raise ValueError("collision page directory totals do not match header")

        plane_counts = [0, 0, 0, 0]
        for mapsquare, (_, count) in enumerate(index):
            last_key = -1
            for _ in range(count):
                raw = handle.read(RECORD.size)
                if len(raw) != RECORD.size:
                    raise ValueError("short collision page record")
                local_x, local_y, plane, pad, flags = RECORD.unpack(raw)
                key = plane * 4096 + local_x * 64 + local_y
                if local_x >= 64 or local_y >= 64 or plane >= 4:
                    raise ValueError("invalid collision tile coordinate")
                if pad != 0 or flags == 0:
                    raise ValueError("invalid collision tile payload")
                if key <= last_key:
                    raise ValueError("collision page rows are duplicate or unsorted")
                last_key = key
                plane_counts[plane] += 1
                yield mapsquare, local_x, local_y, plane, flags
        if tuple(plane_counts) != tuple(header[5:9]):
            raise ValueError("collision row plane counts do not match header")
