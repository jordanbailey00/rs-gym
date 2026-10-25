#!/usr/bin/env python3
"""Export aggregate or per-mapsquare visual scenes from b237."""

from __future__ import annotations

import argparse
import struct
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from region_sets import around_tile

from export_objects import export_modern_objects, export_modern_objects_split
from export_minimap import export_minimap_split
from export_terrain import export_modern_terrain, export_modern_terrain_split
from rc_cache import RcCacheStore, find_all_map_region_files
from source_inputs import B237_CACHE, require_path


MAPSQUARE_CATALOG_MAGIC = 0x3151534D
MAPSQUARE_CATALOG_NAME = "mapsquare.catalog"


def partition_regions(
    regions: list[tuple[int, int]], jobs: int
) -> list[list[tuple[int, int]]]:
    """Split row-major regions into bounded contiguous worker batches."""
    if jobs < 1:
        raise ValueError("jobs must be >= 1")
    worker_count = min(jobs, len(regions))
    if worker_count == 0:
        return []
    batch_size, remainder = divmod(len(regions), worker_count)
    batches: list[list[tuple[int, int]]] = []
    offset = 0
    for index in range(worker_count):
        count = batch_size + (1 if index < remainder else 0)
        batches.append(regions[offset : offset + count])
        offset += count
    return batches


def export_split_batch(
    cache: Path,
    regions: list[tuple[int, int]],
    output_dir: Path,
    planes: list[int],
    write_shared_materials: bool,
) -> tuple[int, int]:
    terrain_outputs = export_modern_terrain_split(
        cache,
        regions,
        output_dir,
        planes,
    )
    object_outputs = export_modern_objects_split(
        cache,
        regions,
        output_dir,
        planes,
        rsmod_visual_levels=True,
        write_shared_materials=write_shared_materials,
    )
    export_minimap_split(cache, regions, output_dir, planes)
    return len(terrain_outputs), len(object_outputs)


def plane_path(prefix: Path, plane: int, suffix: str) -> Path:
    if plane == 0:
        return prefix.with_suffix(suffix)
    return prefix.with_name(f"{prefix.name}.p{plane}{suffix}")


def parse_planes(raw: str) -> list[int]:
    planes = list(
        dict.fromkeys(int(value) for value in raw.split(",") if value.strip())
    )
    if not planes or any(plane < 0 or plane > 3 for plane in planes):
        raise ValueError("--planes values must be in 0..3")
    return planes


def parse_regions(raw: str) -> list[tuple[int, int]]:
    regions: set[tuple[int, int]] = set()
    for token in raw.split():
        parts = token.split(",")
        if len(parts) != 2:
            raise ValueError(f"invalid mapsquare coordinate: {token}")
        region = (int(parts[0]), int(parts[1]))
        if not 0 <= region[0] <= 255 or not 0 <= region[1] <= 255:
            raise ValueError(f"mapsquare coordinate out of range: {token}")
        regions.add(region)
    if not regions:
        raise ValueError("--regions must contain at least one mapsquare")
    return sorted(regions, key=lambda region: (region[1], region[0]))


def write_mapsquare_catalog(
    cache: Path,
    output_dir: Path,
) -> set[tuple[int, int]]:
    """Write the authoritative cache-backed visual mapsquare set."""
    entries = find_all_map_region_files(RcCacheStore(cache))
    regions = sorted(
        {
            (entry.region_x, entry.region_y)
            for entry in entries.values()
            if entry.has_terrain
        },
        key=lambda region: (region[1], region[0]),
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    payload = bytearray(struct.pack("<II", MAPSQUARE_CATALOG_MAGIC, len(regions)))
    for region_x, region_y in regions:
        payload.extend(struct.pack("<BB", region_x, region_y))
    output = output_dir / MAPSQUARE_CATALOG_NAME
    temporary = output.with_name(f".{output.name}.tmp")
    temporary.write_bytes(payload)
    temporary.replace(output)
    return set(regions)


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        description="export aggregate or mapsquare-split visual scenes from b237"
    )
    parser.add_argument("--center-x", type=int)
    parser.add_argument("--center-y", type=int)
    parser.add_argument("--radius-regions", type=int, default=2)
    parser.add_argument(
        "--regions",
        help='space-separated mapsquares, for example "50,53 51,53"',
    )
    parser.add_argument(
        "--cache",
        type=Path,
        default=B237_CACHE,
    )
    parser.add_argument("--output-prefix", type=Path)
    parser.add_argument("--split-by-mapsquare", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--planes", type=str, default="0,1,2,3")
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="parallel worker processes for split mapsquare export (default: 1)",
    )
    args = parser.parse_args(argv)

    cache = require_path(args.cache, "--cache", "RUNEC_B237_CACHE")
    if not cache.is_dir():
        parser.error(f"cache directory not found: {cache}")
    if args.radius_regions < 0:
        parser.error("--radius-regions must be >= 0")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    if not args.split_by_mapsquare and args.jobs != 1:
        parser.error("--jobs is only valid with --split-by-mapsquare")

    has_center = args.center_x is not None or args.center_y is not None
    if has_center and (args.center_x is None or args.center_y is None):
        parser.error("--center-x and --center-y must be provided together")
    if args.regions and has_center:
        parser.error("use either --regions or --center-x/--center-y")
    if not args.regions and not has_center:
        parser.error("provide --regions or --center-x/--center-y")

    try:
        planes = parse_planes(args.planes)
        regions = (
            parse_regions(args.regions)
            if args.regions
            else around_tile(args.center_x, args.center_y, args.radius_regions)
        )
    except ValueError as exc:
        parser.error(str(exc))

    print(
        f"scene_slice: exporting {len(regions)} regions from {cache}",
        file=sys.stderr,
    )

    if args.split_by_mapsquare:
        if args.output_dir is None:
            parser.error("--split-by-mapsquare requires --output-dir")
        if args.output_prefix is not None:
            parser.error("--output-prefix is only valid for aggregate exports")
        available_regions = write_mapsquare_catalog(cache, args.output_dir)
        selected_regions = [
            region for region in regions if region in available_regions
        ]
        void_count = len(regions) - len(selected_regions)
        if void_count:
            print(
                f"scene_slice: skipped {void_count} authoritative void "
                f"mapsquare{'s' if void_count != 1 else ''}",
                file=sys.stderr,
            )
        if not selected_regions:
            print(
                "scene_slice: wrote mapsquare catalog; no requested regions "
                "contain terrain",
                file=sys.stderr,
            )
            return
        if args.jobs == 1 or len(selected_regions) == 1:
            terrain_count, object_count = export_split_batch(
                cache,
                selected_regions,
                args.output_dir,
                planes,
                True,
            )
        else:
            batches = partition_regions(selected_regions, args.jobs)
            print(
                f"scene_slice: exporting with {len(batches)} bounded workers",
                file=sys.stderr,
            )
            terrain_count = 0
            object_count = 0
            with ProcessPoolExecutor(max_workers=len(batches)) as executor:
                futures = [
                    executor.submit(
                        export_split_batch,
                        cache,
                        batch,
                        args.output_dir,
                        planes,
                        index == 0,
                    )
                    for index, batch in enumerate(batches)
                ]
                for future in as_completed(futures):
                    batch_terrain_count, batch_object_count = future.result()
                    terrain_count += batch_terrain_count
                    object_count += batch_object_count
        print(
            f"scene_slice: wrote {terrain_count} terrain and "
            f"{object_count} object/material files plus "
            f"{len(selected_regions) * len(planes)} minimaps and catalog",
            file=sys.stderr,
        )
        return

    if args.output_prefix is None:
        parser.error("aggregate exports require --output-prefix")
    if args.output_dir is not None:
        parser.error("--output-dir requires --split-by-mapsquare")
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)

    for plane in planes:
        terrain = plane_path(args.output_prefix, plane, ".terrain")
        objects = plane_path(args.output_prefix, plane, ".objects")
        print(f"scene_slice: plane {plane} terrain -> {terrain}", file=sys.stderr)
        export_modern_terrain(
            cache_dir=cache,
            regions=regions,
            output=terrain,
            scene_plane=plane,
        )
        print(f"scene_slice: plane {plane} objects -> {objects}", file=sys.stderr)
        export_modern_objects(
            cache_dir=cache,
            regions=regions,
            output=objects,
            scene_plane=plane,
            rsmod_visual_levels=True,
        )


if __name__ == "__main__":
    main()
