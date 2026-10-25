#!/usr/bin/env python3
"""Emit full-world sparse collision tiles from the local b237 cache."""
from __future__ import annotations

import sys
import argparse
from collections import Counter
from pathlib import Path

from source_paths import CACHE_DIR, display_path, require_cache_dir

PIPELINE = Path(__file__).resolve().parent / "cache_pipeline"
sys.path.insert(0, str(PIPELINE))

from export_collision_map import new_collision_flags, parse_terrain  # noqa: E402
from export_collision_map_modern import (  # noqa: E402
    decode_modern_obj_defs_rc,
    parse_objects_modern,
)
from rc_cache import RcCacheStore, find_all_map_region_files, read_map_region_file  # noqa: E402

from collision_tile_index import (  # noqa: E402
    COLLISION_TILE_INDEX_MAGIC,
    COLLISION_TILE_INDEX_VERSION,
    COLLISION_TILE_MAPSQUARE_COUNT,
    HEADER,
    INDEX_ENTRY,
    RECORD,
)

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data/regions/world.collision-tiles.indexed.bin"
REPORT = ROOT / "build/reports/collision_tiles.txt"


def nonzero_rows(flags: list[list[list[int]]]) -> list[tuple[int, int, int, int]]:
    rows: list[tuple[int, int, int, int]] = []
    for plane in range(4):
        for lx in range(64):
            for ly in range(64):
                value = flags[plane][lx][ly]
                if value:
                    rows.append((lx, ly, plane, value))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=OUT)
    parser.add_argument("--report", type=Path, default=REPORT)
    parser.add_argument("--limit-regions", type=int, default=0)
    parser.add_argument("--cache", type=Path, default=CACHE_DIR)
    args = parser.parse_args()
    cache_dir = require_cache_dir(args.cache)

    store = RcCacheStore(cache_dir)
    obj_defs = decode_modern_obj_defs_rc(store)
    map_regions = find_all_map_region_files(store)
    plane_counts: Counter[int] = Counter()
    region_count = 0
    row_count = 0
    error_count = 0
    collision_objects = 0

    targets = sorted(map_regions.items())
    if args.limit_regions > 0:
        targets = targets[:args.limit_regions]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    tmp_output = args.output.with_name(args.output.name + ".tmp")
    tmp_output.unlink(missing_ok=True)
    page_counts = [0] * COLLISION_TILE_MAPSQUARE_COUNT
    try:
        with tmp_output.open("w+b") as output:
            output.write(
                b"\0" * (
                    HEADER.size
                    + COLLISION_TILE_MAPSQUARE_COUNT * INDEX_ENTRY.size
                )
            )
            for ms, region in targets:
                terrain = None
                loc = None
                try:
                    if region.has_terrain:
                        terrain = read_map_region_file(
                            store, region.region_x, region.region_y, "terrain"
                        )
                    if region.has_locations:
                        loc = read_map_region_file(
                            store, region.region_x, region.region_y, "locations"
                        )
                except Exception:
                    error_count += 1
                    continue
                if terrain is None and loc is None:
                    error_count += 1
                    continue
                flags = new_collision_flags()
                down_heights: set[tuple[int, int, int]] = set()
                if terrain:
                    try:
                        flags, down_heights = parse_terrain(terrain)
                    except Exception:
                        error_count += 1
                        flags = new_collision_flags()
                        down_heights = set()
                if loc:
                    try:
                        collision_objects += parse_objects_modern(
                            loc, flags, down_heights, obj_defs
                        )
                    except Exception:
                        error_count += 1
                region_rows = nonzero_rows(flags)
                page_counts[ms] = len(region_rows)
                if region_rows:
                    region_count += 1
                for lx, ly, plane, value in region_rows:
                    plane_counts[plane] += 1
                    output.write(RECORD.pack(lx, ly, plane, 0, value & 0xFFFFFFFF))
                row_count += len(region_rows)

            output.seek(0)
            output.write(HEADER.pack(
                COLLISION_TILE_INDEX_MAGIC,
                COLLISION_TILE_INDEX_VERSION,
                row_count,
                RECORD.size,
                region_count,
                *(plane_counts[plane] for plane in range(4)),
            ))
            first = 0
            for count in page_counts:
                output.write(INDEX_ENTRY.pack(first, count))
                first += count
        tmp_output.replace(args.output)
    except BaseException:
        tmp_output.unlink(missing_ok=True)
        raise

    lines = [
        "Collision tile catalog",
        "",
        "status: READY_WITH_ACCEPTED_SIMPLIFICATIONS",
        f"output binary: {args.output.relative_to(ROOT) if args.output.is_relative_to(ROOT) else args.output} ({args.output.stat().st_size} bytes)",
        f"source cache: {display_path(cache_dir)}",
        "source object defs: b237 cache index 2 group 6",
        f"cache map groups: {len(map_regions)}",
        f"exported map groups: {len(targets)}",
        f"regions with collision: {region_count}",
        f"non-zero collision tiles: {row_count}",
        f"collision-marked objects: {collision_objects}",
        f"region parse/read errors: {error_count}",
        "",
        "by plane:",
    ]
    for plane in range(4):
        lines.append(f"  plane {plane}: {plane_counts[plane]}")
    lines.extend([
        "",
        "accepted simplifications:",
        "  - sparse rows store non-zero collision flags only; runtime expands active mapsquare pages to dense lookup",
        "  - a fixed mapsquare directory supports bounded loose/pack range reads without full-file extraction",
        "  - area semantics such as wilderness/multi/safe-zone are a separate area-flag dataset",
    ])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n")
    print(args.report.read_text())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
