"""Mapsquare-indexed object placement binary helpers."""
from __future__ import annotations

import struct
from collections.abc import Iterator
from pathlib import Path


OBJECT_PLACEMENT_INDEX_MAGIC = int.from_bytes(b"OPLI", "little")
OBJECT_PLACEMENT_INDEX_VERSION = 1
OBJECT_PLACEMENT_MAPSQUARE_COUNT = 1 << 16

HEADER = struct.Struct("<9I")
INDEX_ENTRY = struct.Struct("<II")
RECORD = struct.Struct("<IQHHHBBBB")


def _read_exact(handle, size: int, path: Path) -> bytes:
    data = handle.read(size)
    if len(data) != size:
        raise ValueError(f"{path}: truncated object placement index")
    return data


def read_header(path: Path) -> tuple[int, ...]:
    with path.open("rb") as handle:
        header = HEADER.unpack(_read_exact(handle, HEADER.size, path))
    magic, version, _rows, record_size, _pages, *_planes = header
    if magic != OBJECT_PLACEMENT_INDEX_MAGIC:
        raise ValueError(f"{path}: bad object placement index magic")
    if version != OBJECT_PLACEMENT_INDEX_VERSION:
        raise ValueError(f"{path}: unsupported object placement index version")
    if record_size != RECORD.size:
        raise ValueError(f"{path}: unexpected object placement record size")
    return header


def iter_object_placements(path: Path) -> Iterator[tuple[int, ...]]:
    with path.open("rb") as handle:
        header = HEADER.unpack(_read_exact(handle, HEADER.size, path))
        magic, version, row_count, record_size, occupied_pages, *planes = header
        if magic != OBJECT_PLACEMENT_INDEX_MAGIC:
            raise ValueError(f"{path}: bad object placement index magic")
        if version != OBJECT_PLACEMENT_INDEX_VERSION:
            raise ValueError(
                f"{path}: unsupported object placement index version"
            )
        if record_size != RECORD.size:
            raise ValueError(
                f"{path}: unexpected object placement record size"
            )

        entries = [
            INDEX_ENTRY.unpack(_read_exact(handle, INDEX_ENTRY.size, path))
            for _ in range(OBJECT_PLACEMENT_MAPSQUARE_COUNT)
        ]
        expected_first = 0
        actual_pages = 0
        for first, count in entries:
            if first != expected_first:
                raise ValueError(f"{path}: non-contiguous placement page index")
            expected_first += count
            actual_pages += count > 0
        if expected_first != row_count or actual_pages != occupied_pages:
            raise ValueError(f"{path}: inconsistent placement page counts")

        actual_planes = [0, 0, 0, 0]
        for mapsquare, (_first, count) in enumerate(entries):
            page = _read_exact(handle, count * RECORD.size, path)
            for row in RECORD.iter_unpack(page):
                _obj_id, key, x, y, stored_mapsquare, plane, typ, rotation, _flags = row
                if key == 0:
                    raise ValueError(f"{path}: zero placement key")
                if stored_mapsquare != mapsquare:
                    raise ValueError(f"{path}: placement stored in wrong mapsquare")
                if ((x >> 6) << 8 | (y >> 6)) != mapsquare:
                    raise ValueError(f"{path}: placement coordinates mismatch page")
                if plane >= 4 or typ >= 64 or rotation >= 4:
                    raise ValueError(f"{path}: invalid placement row")
                actual_planes[plane] += 1
                yield row
        if handle.read(1):
            raise ValueError(f"{path}: trailing object placement bytes")
        if actual_planes != list(planes):
            raise ValueError(f"{path}: inconsistent placement plane counts")
