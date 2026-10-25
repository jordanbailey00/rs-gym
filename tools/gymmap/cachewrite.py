"""Write an overlay cache: the source cache with its map index (5) replaced by gym mapsquares."""

from __future__ import annotations

import os
import shutil
import struct
import zlib
from pathlib import Path

MAP_INDEX = 5
MANIFEST_INDEX = 255
TERRAIN_FILE = 0
LOCS_FILE = 1


def container(payload: bytes) -> bytes:
    """Uncompressed RS2 container: compression byte 0, u32 length, payload."""
    return b"\x00" + struct.pack(">I", len(payload)) + payload


def map_group(terrain: bytes, locs: bytes) -> bytes:
    """Two-file group (terrain, locs) in one chunk; the size table holds per-file deltas."""
    return terrain + locs + struct.pack(">ii", len(terrain), len(locs) - len(terrain)) + b"\x01"


def _big_smart(value: int) -> bytes:
    return struct.pack(">H", value) if value < 0x8000 else struct.pack(">I", value | 0x80000000)


def map_manifest(containers: dict[int, bytes]) -> bytes:
    """Protocol-7 reference table with no names, digests or lengths (flags 0)."""
    ids = sorted(containers)
    out = bytearray([7]) + struct.pack(">I", 0) + b"\x00" + _big_smart(len(ids))
    prev = 0
    for gid in ids:
        out += _big_smart(gid - prev)
        prev = gid
    for gid in ids:
        out += struct.pack(">I", zlib.crc32(containers[gid]))
    for _ in ids:
        out += struct.pack(">I", 0)  # group revision
    for _ in ids:
        out += _big_smart(2)
    for _ in ids:
        out += _big_smart(TERRAIN_FILE) + _big_smart(LOCS_FILE - TERRAIN_FILE)
    return bytes(out)


def write_overlay_cache(source: Path, out: Path, mapsquares: dict[tuple[int, int], tuple[bytes, bytes]]) -> None:
    """Link every index except the map index back to `source`; write gym mapsquares as index 5."""
    if not (source / str(MANIFEST_INDEX)).is_dir():
        raise SystemExit(f"not an OpenRS2 flat cache: {source}")
    if out.exists():
        shutil.rmtree(out)
    (out / str(MAP_INDEX)).mkdir(parents=True)
    (out / str(MANIFEST_INDEX)).mkdir()

    for entry in source.iterdir():
        if entry.name in (str(MAP_INDEX), str(MANIFEST_INDEX)):
            continue
        os.symlink(os.path.relpath(entry, out), out / entry.name)
    for entry in (source / str(MANIFEST_INDEX)).iterdir():
        if entry.name != f"{MAP_INDEX}.dat":
            os.symlink(os.path.relpath(entry, out / str(MANIFEST_INDEX)), out / str(MANIFEST_INDEX) / entry.name)

    containers = {}
    for (rx, ry), (terrain, locs) in mapsquares.items():
        gid = (rx << 8) | ry
        containers[gid] = container(map_group(terrain, locs))
        (out / str(MAP_INDEX) / f"{gid}.dat").write_bytes(containers[gid])
    (out / str(MANIFEST_INDEX) / f"{MAP_INDEX}.dat").write_bytes(container(map_manifest(containers)))
