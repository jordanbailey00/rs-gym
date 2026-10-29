#!/usr/bin/env python3
"""Build a gym's runtime data: compile the map spec, then run the exporters on it.

Steps: spec -> mapsquares -> overlay cache (build/gym/cache) -> object placements -> gathering nodes
-> data/regions visuals and collision -> spawned-loc templates -> data/spawns -> traversal, area-flag
and drop tables -> manifest.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tomllib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "cache_pipeline"))

from gymmap.cachewrite import write_overlay_cache  # noqa: E402
from gymmap.compile import compile_gym  # noqa: E402
from gymmap.drops import merge_gym_drops  # noqa: E402
from gymmap.nodes import write_gathering_nodes  # noqa: E402
from gymmap.spatial import write_area_flags, write_traversal  # noqa: E402
from spawn_index import write_ground_item_spawns, write_npc_spawns  # noqa: E402

SOURCE_CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
SOURCE_EDGES = ROOT / "data/source/traversal_edges.b237.bin"  # real-world stairs and ladders
SOURCE_DROPS = ROOT / "data/source/drops.b237.bin"
OVERLAY_CACHE = ROOT / "build/gym/cache"
DATA = ROOT / "data"


def run(script: str, *args) -> None:
    subprocess.run([sys.executable, str(ROOT / script), *map(str, args)], check=True, cwd=ROOT)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", nargs="?", type=Path, default=ROOT / "maps/gym_v1.toml")
    args = parser.parse_args()
    if not SOURCE_CACHE.is_dir():
        raise SystemExit(f"missing source cache: {SOURCE_CACHE}")

    gym = compile_gym(args.spec, SOURCE_CACHE, SOURCE_EDGES)
    write_overlay_cache(SOURCE_CACHE, OVERLAY_CACHE, gym.mapsquares)
    print(f"compiled {args.spec.name}: {len(gym.mapsquares)} mapsquares, {len(gym.npc_spawns)} npcs, "
          f"{len(gym.ground_items)} ground items, {len(gym.edges)} traversals, {len(gym.zones)} zone rows", flush=True)

    regions = DATA / "regions"
    shutil.rmtree(regions, ignore_errors=True)  # every file here is generated from the spec
    regions.mkdir(parents=True)
    # Placements first: gathering nodes carry their placement keys, and the scene export reads the
    # nodes to give trees and rocks depletable (dynamic) meshes.
    run("tools/export_object_placements.py", "--cache", OVERLAY_CACHE)
    nodes = write_gathering_nodes(regions / "world.object-placements.indexed.bin",
                                  DATA / "defs/object_behaviors.bin", DATA / "defs/gathering_nodes.bin")
    print(f"gathering nodes: {nodes}", flush=True)
    squares = " ".join(f"{x},{y}" for x, y in sorted(gym.mapsquares))
    run("tools/cache_pipeline/export_scene_slice.py", "--split-by-mapsquare", "--regions", squares,
        "--planes", "0,1,2,3", "--output-dir", regions, "--cache", OVERLAY_CACHE)
    run("tools/export_collision_tiles.py", "--cache", OVERLAY_CACHE)
    run("tools/cache_pipeline/export_spawn_objects.py", args.spec, "--cache", SOURCE_CACHE)

    write_npc_spawns(DATA / "spawns/world.npc-spawns.indexed.bin", gym.npc_spawns)
    write_ground_item_spawns(DATA / "spawns/world.ground-items.indexed.bin", gym.ground_items)
    write_traversal(DATA / "defs/traversal_edges.bin", gym.edges)
    write_area_flags(DATA / "defs/area_flags.bin", gym.zones)
    write_layout(gym, regions / "gym.cfg")
    spec = tomllib.loads(args.spec.read_text())
    merged = merge_gym_drops(SOURCE_DROPS, DATA / "defs/drops.bin", spec.get("drops", []))
    print(f"drop tables: {merged} gym tables merged", flush=True)
    (DATA / "manifest.json").write_text(json.dumps(
        {"format": "runec-data-manifest-v1", "data_version": "v1", "gym": gym.name}) + "\n")

    x, y, plane = gym.start
    print(f"gym ready: start tile {x},{y},{plane}; run ./build/cmake/rc-viewer from {ROOT}")


def write_layout(gym, path: Path) -> None:
    """Start, respawn, the mapsquare rect the engine keeps loaded (every playable area) and the
    named landmarks agents can travel to."""
    rxs = [x for a in gym.areas.values() for x in (a.rx, a.rx + a.w - 1)]
    rys = [y for a in gym.areas.values() for y in (a.ry, a.ry + a.h - 1)]
    rect = (min(rxs), min(rys), max(rxs), max(rys))
    if (rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1) > 32:  # RC_MAX_REGIONS
        raise SystemExit(f"areas span {rect}: more than the engine's 32 loaded mapsquares")
    landmarks = "".join(f"landmark {name} {x} {y} {plane}\n" for name, x, y, plane in gym.landmarks)
    path.write_text(f"# written by tools/build_gym.py\nname {gym.name}\n"
                    f"start {' '.join(map(str, gym.start))}\nrespawn {' '.join(map(str, gym.respawn))}\n"
                    f"area {' '.join(map(str, rect))}\n{landmarks}")


if __name__ == "__main__":
    main()
