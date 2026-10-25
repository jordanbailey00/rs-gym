#!/usr/bin/env python3
"""Build a gym's runtime data: compile the map spec, then run the exporters on it.

Steps: spec -> mapsquares -> overlay cache (build/gym/cache) -> data/regions visuals,
collision and object placements -> data/spawns -> data/defs/area_flags.bin -> data/manifest.json.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "cache_pipeline"))

from gymmap.cachewrite import write_overlay_cache  # noqa: E402
from gymmap.compile import compile_gym  # noqa: E402
from spawn_index import write_ground_item_spawns, write_npc_spawns  # noqa: E402

SOURCE_CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
OVERLAY_CACHE = ROOT / "build/gym/cache"
DATA = ROOT / "data"
AREA_FLAGS_MAGIC = int.from_bytes(b"AFLG", "little")


def run(script: str, *args) -> None:
    subprocess.run([sys.executable, str(ROOT / script), *map(str, args)], check=True, cwd=ROOT)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", nargs="?", type=Path, default=ROOT / "maps/gym_v0.toml")
    args = parser.parse_args()
    if not SOURCE_CACHE.is_dir():
        raise SystemExit(f"missing source cache: {SOURCE_CACHE}")

    gym = compile_gym(args.spec, SOURCE_CACHE)
    write_overlay_cache(SOURCE_CACHE, OVERLAY_CACHE, gym.mapsquares)
    print(f"compiled {args.spec.name}: {len(gym.mapsquares)} mapsquares, {len(gym.npc_spawns)} npc spawns", flush=True)

    regions = DATA / "regions"
    shutil.rmtree(regions, ignore_errors=True)  # every file here is generated from the spec
    regions.mkdir(parents=True)
    squares = " ".join(f"{x},{y}" for x, y in sorted(gym.mapsquares))
    run("tools/cache_pipeline/export_scene_slice.py", "--split-by-mapsquare", "--regions", squares,
        "--planes", "0,1,2,3", "--output-dir", regions, "--cache", OVERLAY_CACHE)
    run("tools/export_collision_tiles.py", "--cache", OVERLAY_CACHE)
    run("tools/export_object_placements.py", "--cache", OVERLAY_CACHE)

    write_npc_spawns(DATA / "spawns/world.npc-spawns.indexed.bin", gym.npc_spawns)
    write_ground_item_spawns(DATA / "spawns/world.ground-items.indexed.bin", [])
    # No multi-combat or wilderness zones in v0: an empty AFLG table (header only).
    (DATA / "defs/area_flags.bin").write_bytes(struct.pack("<4I", AREA_FLAGS_MAGIC, 1, 0, 0))
    (DATA / "manifest.json").write_text(json.dumps(
        {"format": "runec-data-manifest-v1", "data_version": "v1", "gym": gym.name}) + "\n")

    x, y, plane = gym.start
    print(f"gym ready: start tile {x},{y},{plane}; run ./build/cmake/rc-viewer from {ROOT}")


if __name__ == "__main__":
    main()
