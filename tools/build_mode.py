#!/usr/bin/env python3
"""Build a mode's runtime data into data/modes/<mode>/.

    python3 tools/build_mode.py [mode]        # default: compact_osrs_v1

Steps: map spec -> mapsquares -> overlay cache (build/modes/<mode>/cache) -> object placements ->
mode rules (modes/<mode>/*.toml, when the mode has them) -> gathering nodes -> regions (terrain,
objects, minimaps, collision) -> spawned-loc templates -> spawns -> traversal, area-flag and drop
tables -> gym.cfg layout -> content manifest and override ledger (build/cmake/rs-gym-validate).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "cache_pipeline"))

from gymmap.cachewrite import write_overlay_cache  # noqa: E402
from gymmap.compile import compile_gym  # noqa: E402
from gymmap.drops import merge_gym_drops, write_drops  # noqa: E402
from gymmap.mapformat import PLANES, SIZE, decode_terrain  # noqa: E402
from gymmap.nodes import write_gathering_nodes  # noqa: E402
from gymmap.spatial import write_area_flags, write_traversal  # noqa: E402
from spawn_index import write_ground_item_spawns, write_npc_spawns  # noqa: E402

SOURCE_CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
SOURCE_EDGES = ROOT / "data/source/traversal_edges.b237.bin"  # real-world stairs and ladders
SOURCE_DROPS = ROOT / "data/source/drops.b237.bin"
DATA = ROOT / "data"
DEFAULT_MODE = "compact_osrs_v1"


def run(script: str, *args, env: dict | None = None) -> None:
    subprocess.run([sys.executable, str(ROOT / script), *map(str, args)], check=True, cwd=ROOT,
                   env={**os.environ, **(env or {})})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", nargs="?", default=DEFAULT_MODE)
    args = parser.parse_args()
    if not SOURCE_CACHE.is_dir():
        raise SystemExit(f"missing source cache: {SOURCE_CACHE}")
    mode_dir = ROOT / "modes" / args.mode
    if not (mode_dir / "mode.toml").is_file():
        raise SystemExit(f"unknown mode {args.mode!r}: no {mode_dir / 'mode.toml'}")
    mode = tomllib.loads((mode_dir / "mode.toml").read_text())["mode"]
    name = mode["name"]
    map_path = ROOT / mode["map"]
    out = DATA / "modes" / name
    cache = ROOT / "build" / "modes" / name / "cache"

    gym = compile_gym(map_path, SOURCE_CACHE, SOURCE_EDGES)
    write_overlay_cache(SOURCE_CACHE, cache, gym.mapsquares)
    print(f"compiled {map_path.name}: {len(gym.mapsquares)} mapsquares, {len(gym.npc_spawns)} npcs, "
          f"{len(gym.ground_items)} ground items, {len(gym.edges)} traversals, {len(gym.zones)} zone rows",
          flush=True)

    shutil.rmtree(out, ignore_errors=True)  # every file here is generated
    for sub in ("regions", "defs", "spawns", "models"):
        (out / sub).mkdir(parents=True)
    placements = out / "regions/world.object-placements.indexed.bin"
    run("tools/export_object_placements.py", "--cache", cache, "--output", placements)

    rules = None
    if (mode_dir / "content.toml").is_file():
        from modec import compile_mode  # noqa: E402
        rules = compile_mode(mode_dir, out, placements, gym)
        print(f"mode rules: {out.relative_to(ROOT) / 'mode.rules'}, manifest "
              f"{(mode_dir / 'manifest.json').relative_to(ROOT)}", flush=True)
    nodes_path = out / "defs/gathering_nodes.bin"
    nodes = write_gathering_nodes(placements, DATA / "defs/object_behaviors.bin", nodes_path,
                                  rules.node_respawn if rules else None,
                                  rules.node_depleted if rules else None,
                                  rules.extra_nodes if rules else None)
    print(f"gathering nodes: {nodes}", flush=True)

    squares = " ".join(f"{x},{y}" for x, y in sorted(gym.mapsquares))
    run("tools/cache_pipeline/export_scene_slice.py", "--split-by-mapsquare", "--regions", squares,
        "--planes", "0,1,2,3", "--output-dir", out / "regions", "--cache", cache,
        env={"RS_GYM_GATHERING_NODES": str(nodes_path)})
    run("tools/export_collision_tiles.py", "--cache", cache,
        "--output", out / "regions/world.collision-tiles.indexed.bin",
        "--report", ROOT / "build/reports" / f"{name}-collision-tiles.txt")
    run("tools/cache_pipeline/export_spawn_objects.py", map_path, "--cache", SOURCE_CACHE,
        "--output", out / "models/spawn_objects.models")

    write_roofs(gym, out / "regions/roofs.bin")
    write_npc_spawns(out / "spawns/world.npc-spawns.indexed.bin", gym.npc_spawns)
    write_ground_item_spawns(out / "spawns/world.ground-items.indexed.bin", gym.ground_items)
    write_traversal(out / "defs/traversal_edges.bin", gym.edges)
    write_area_flags(out / "defs/area_flags.bin", gym.zones)
    write_layout(gym, out / "gym.cfg", name)
    if rules:
        write_drops(out / "defs/drops.bin", rules.drops)  # closed: only the mode's tables
        print(f"drop tables: {len(rules.drops)} mode tables", flush=True)
    else:
        spec = tomllib.loads(map_path.read_text())
        merged = merge_gym_drops(SOURCE_DROPS, out / "defs/drops.bin", spec.get("drops", []))
        print(f"drop tables: {merged} gym tables merged", flush=True)
    manifest = DATA / "manifest.json"
    if not manifest.exists():
        manifest.write_text(json.dumps({"format": "runec-data-manifest-v1", "data_version": "v1"}) + "\n")

    validator = ROOT / "build/cmake/rs-gym-validate"
    if rules and validator.exists():
        subprocess.run([str(validator), name], check=True, cwd=ROOT)
    elif rules:
        print("build the C targets, then run ./build/cmake/rs-gym-validate", name, "for the manifest")
    x, y, plane = gym.start
    print(f"mode {name} ready in {out.relative_to(ROOT)}: start tile {x},{y},{plane}")


def write_roofs(gym, path: Path) -> None:
    """The tiles under a roof (tile setting 4) per mapsquare and plane, as bitmaps: the viewer
    draws roofs and upper floors only while the player stands outside them."""
    rows = []
    for (rx, ry), (terrain, _) in sorted(gym.mapsquares.items()):
        tiles = decode_terrain(terrain)
        bits = bytearray(PLANES * SIZE * SIZE // 8)
        for plane in range(PLANES):
            for x in range(SIZE):
                for y in range(SIZE):
                    if tiles[plane][x][y].settings & 4:
                        k = plane * SIZE * SIZE + x * SIZE + y
                        bits[k >> 3] |= 1 << (k & 7)
        if any(bits):
            rows.append(struct.pack("<ii", rx, ry) + bytes(bits))
    path.write_bytes(struct.pack("<II", 0x464F4F52, len(rows)) + b"".join(rows))


def write_layout(gym, path: Path, mode: str) -> None:
    """Mode name, start, respawn, the mapsquare rect the engine keeps loaded (every playable area)
    and the named landmarks."""
    rxs = [x for a in gym.areas.values() for x in (a.rx, a.rx + a.w - 1)]
    rys = [y for a in gym.areas.values() for y in (a.ry, a.ry + a.h - 1)]
    rect = (min(rxs), min(rys), max(rxs), max(rys))
    if (rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1) > 32:  # RC_MAX_REGIONS
        raise SystemExit(f"areas span {rect}: more than the engine's 32 loaded mapsquares")
    landmarks = "".join(f"landmark {n} {x} {y} {plane}\n" for n, x, y, plane in gym.landmarks)
    path.write_text(f"# written by tools/build_mode.py\nname {gym.name}\nmode {mode}\n"
                    f"start {' '.join(map(str, gym.start))}\nrespawn {' '.join(map(str, gym.respawn))}\n"
                    f"area {' '.join(map(str, rect))}\n{landmarks}")


if __name__ == "__main__":
    main()
