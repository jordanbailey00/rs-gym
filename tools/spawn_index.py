"""Mapsquare-indexed spatial spawn binary helpers."""
from __future__ import annotations

import struct
from pathlib import Path
from typing import Sequence


SPAWN_INDEX_VERSION = 1
SPAWN_MAPSQUARE_COUNT = 1 << 16
NPC_SPAWN_INDEX_MAGIC = int.from_bytes(b"NSPI", "little")
GROUND_ITEM_INDEX_MAGIC = int.from_bytes(b"GSPI", "little")

HEADER = struct.Struct("<9I")
INDEX_ENTRY = struct.Struct("<II")
NPC_PAYLOAD = struct.Struct("<IiiBBBB")
GROUND_ITEM_PAYLOAD = struct.Struct("<IIiiBBI")


def _mapsquare(x: int, y: int) -> int:
    if x < 0 or x >= 16384 or y < 0 or y >= 16384:
        raise ValueError(f"spawn coordinate outside mapsquare index: {x},{y}")
    return ((x >> 6) << 8) | (y >> 6)


def _write_indexed(
    path: Path,
    magic: int,
    payload_struct: struct.Struct,
    rows: Sequence[tuple[int, ...]],
    x_index: int,
    y_index: int,
    plane_index: int,
) -> None:
    pages: dict[int, list[tuple[int, bytes]]] = {}
    plane_counts = [0, 0, 0, 0]
    for source_order, row in enumerate(rows):
        x = int(row[x_index])
        y = int(row[y_index])
        plane = int(row[plane_index])
        if plane < 0 or plane >= 4:
            raise ValueError(f"spawn plane outside 0..3: {plane}")
        page = _mapsquare(x, y)
        pages.setdefault(page, []).append(
            (source_order, payload_struct.pack(*row))
        )
        plane_counts[plane] += 1

    path.parent.mkdir(parents=True, exist_ok=True)
    record_size = 4 + payload_struct.size
    with path.open("wb") as handle:
        handle.write(HEADER.pack(
            magic,
            SPAWN_INDEX_VERSION,
            len(rows),
            record_size,
            len(pages),
            *plane_counts,
        ))
        first_row = 0
        for mapsquare in range(SPAWN_MAPSQUARE_COUNT):
            page_rows = pages.get(mapsquare, ())
            handle.write(INDEX_ENTRY.pack(first_row, len(page_rows)))
            first_row += len(page_rows)
        for mapsquare in range(SPAWN_MAPSQUARE_COUNT):
            for source_order, payload in pages.get(mapsquare, ()):
                handle.write(struct.pack("<I", source_order))
                handle.write(payload)


def write_npc_spawns(path: Path, rows: Sequence[tuple[int, ...]]) -> None:
    """Write npc_id,x,y,plane,direction,wander,flags rows."""
    _write_indexed(
        path,
        NPC_SPAWN_INDEX_MAGIC,
        NPC_PAYLOAD,
        rows,
        x_index=1,
        y_index=2,
        plane_index=3,
    )


def write_ground_item_spawns(
    path: Path, rows: Sequence[tuple[int, ...]]
) -> None:
    """Write item_id,quantity,x,y,plane,flags,respawn_ticks rows."""
    _write_indexed(
        path,
        GROUND_ITEM_INDEX_MAGIC,
        GROUND_ITEM_PAYLOAD,
        rows,
        x_index=2,
        y_index=3,
        plane_index=4,
    )


def _read_exact(handle, size: int, path: Path) -> bytes:
    data = handle.read(size)
    if len(data) != size:
        raise ValueError(f"{path}: truncated spawn index")
    return data


def read_indexed_header(path: Path, expected_magic: int) -> tuple[int, ...]:
    with path.open("rb") as handle:
        header = HEADER.unpack(_read_exact(handle, HEADER.size, path))
    if header[0] != expected_magic or header[1] != SPAWN_INDEX_VERSION:
        raise ValueError(f"{path}: unsupported spawn index header")
    return header


def _read_indexed(
    path: Path,
    magic: int,
    payload_struct: struct.Struct,
    x_index: int,
    y_index: int,
    plane_index: int,
) -> list[tuple[int, ...]]:
    with path.open("rb") as handle:
        header = HEADER.unpack(_read_exact(handle, HEADER.size, path))
        file_magic, version, row_count, record_size, occupied_pages, *planes = header
        if file_magic != magic or version != SPAWN_INDEX_VERSION:
            raise ValueError(f"{path}: unsupported spawn index header")
        if record_size != 4 + payload_struct.size:
            raise ValueError(f"{path}: unexpected spawn record size {record_size}")
        entries = [
            INDEX_ENTRY.unpack(_read_exact(handle, INDEX_ENTRY.size, path))
            for _ in range(SPAWN_MAPSQUARE_COUNT)
        ]
        expected_first = 0
        actual_pages = 0
        for first, count in entries:
            if first != expected_first:
                raise ValueError(f"{path}: non-contiguous spawn page index")
            expected_first += count
            actual_pages += count > 0
        if expected_first != row_count or actual_pages != occupied_pages:
            raise ValueError(f"{path}: inconsistent spawn page counts")

        ordered: list[tuple[int, ...] | None] = [None] * row_count
        actual_planes = [0, 0, 0, 0]
        for mapsquare, (_first, count) in enumerate(entries):
            for _ in range(count):
                source_order = struct.unpack(
                    "<I", _read_exact(handle, 4, path)
                )[0]
                row = payload_struct.unpack(
                    _read_exact(handle, payload_struct.size, path)
                )
                if source_order >= row_count or ordered[source_order] is not None:
                    raise ValueError(f"{path}: invalid spawn source ordering")
                if _mapsquare(int(row[x_index]), int(row[y_index])) != mapsquare:
                    raise ValueError(f"{path}: spawn stored in wrong mapsquare")
                plane = int(row[plane_index])
                if plane < 0 or plane >= 4:
                    raise ValueError(f"{path}: invalid spawn plane {plane}")
                actual_planes[plane] += 1
                ordered[source_order] = row
        if handle.read(1):
            raise ValueError(f"{path}: trailing spawn index bytes")
        if actual_planes != list(planes):
            raise ValueError(f"{path}: inconsistent spawn plane counts")
        if any(row is None for row in ordered):
            raise ValueError(f"{path}: missing spawn source order")
        return [row for row in ordered if row is not None]


def read_npc_spawns(path: Path) -> list[tuple[int, ...]]:
    return _read_indexed(
        path,
        NPC_SPAWN_INDEX_MAGIC,
        NPC_PAYLOAD,
        x_index=1,
        y_index=2,
        plane_index=3,
    )


def read_ground_item_spawns(path: Path) -> list[tuple[int, ...]]:
    return _read_indexed(
        path,
        GROUND_ITEM_INDEX_MAGIC,
        GROUND_ITEM_PAYLOAD,
        x_index=2,
        y_index=3,
        plane_index=4,
    )
