"""Gathering-node table (GNOD v2) for the gym's trees and rocks, from exported object placements.

Each node row carries the placement key of its loc, so the engine can deplete and respawn that exact
placement, and the loc shown while it is depleted (stump or empty rock). Respawn ticks follow OSRS (oak and willow 8.4 s, maple 35.4 s, yew 59.4 s; copper and tin
2.4 s, iron 5.4 s, coal 29.4 s, mithril 2 min).
"""

from __future__ import annotations

import struct
from pathlib import Path

OPLI_MAGIC = int.from_bytes(b"OPLI", "little")
OBHV_MAGIC = int.from_bytes(b"OBHV", "little")
GNOD_MAGIC = int.from_bytes(b"GNOD", "little")
OBHV_RESOURCE = 0x20
SKILL_WOODCUTTING = 1  # RC_OBJ_SKILL_* (object behaviour skill), not RcSkill
SKILL_MINING = 2

RESPAWN_TICKS = {
    1276: 25, 1278: 25,                      # Tree
    10820: 14,                               # Oak tree
    10819: 14, 10829: 14, 10831: 14, 10833: 14,  # Willow tree
    10832: 59,                               # Maple tree
    10822: 99,                               # Yew tree
    10943: 4, 11161: 4,                      # Copper rocks
    11360: 4, 11361: 4,                      # Tin rocks
    11364: 9, 11365: 9,                      # Iron rocks
    11366: 49, 11367: 49,                    # Coal rocks
    11372: 199, 11373: 199,                  # Mithril rocks
}

# What a node turns into while depleted: the stump or empty rock with the same model shape.
DEPLETED = {
    1276: 1342, 1278: 1342,                                      # tree stump
    10820: 1356,                                                 # oak stump
    10819: 9711, 10829: 9711, 10831: 9711, 10833: 9711,          # willow stump
    10832: 9712,                                                 # maple stump
    10822: 9714,                                                 # yew stump
    10943: 11390, 11360: 11390, 11364: 11390, 11366: 11390, 11372: 11390,  # rocks, model 1390
    11161: 11391, 11361: 11391, 11365: 11391, 11367: 11391, 11373: 11391,  # rocks, model 1391
}


def _behaviors(path: Path) -> dict[int, tuple[int, int, int]]:
    """obj id -> (flags, action_mask, skill) for resource objects."""
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<III", data, 0)
    if magic != OBHV_MAGIC or version != 2:
        raise SystemExit(f"{path}: not an OBHV v2 table")
    out = {}
    for obj, flags, _next, _o, _c, _a, mask, skill, _pad in struct.iter_unpack("<IIiiiiBBH", data[12:12 + 28 * count]):
        if flags & OBHV_RESOURCE:
            out[obj] = (flags, mask, skill)
    return out


def _placements(path: Path):
    data = path.read_bytes()
    magic, version, rows, record = struct.unpack_from("<4I", data, 0)
    if magic != OPLI_MAGIC or version != 1 or record != 22:
        raise SystemExit(f"{path}: not an OPLI v1 index")
    base = 36 + 65536 * 8
    return struct.iter_unpack("<IQHHHBBBB", data[base:base + rows * record])


def write_gathering_nodes(placements: Path, behaviors: Path, out: Path,
                          respawn: dict[int, int] | None = None,
                          depleted: dict[int, int] | None = None) -> int:
    """Node rows for every placed tree and rock. A mode passes its own respawn times (and may
    leave a resource out of `respawn` to keep it out of the table)."""
    respawn = RESPAWN_TICKS if respawn is None else respawn
    depleted = DEPLETED if depleted is None else depleted
    resources = _behaviors(behaviors)
    rows = []
    for obj, key, x, y, mapsquare, plane, loc_type, rotation, _flags in _placements(placements):
        if obj not in resources:
            continue
        flags, mask, skill = resources[obj]
        if skill not in (SKILL_WOODCUTTING, SKILL_MINING):
            continue
        if obj not in respawn:
            raise SystemExit(f"gathering node {obj} at {x},{y} has no respawn time")
        rows.append((obj, key, depleted.get(obj, -1), x, y, mapsquare, plane, skill, mask, loc_type,
                     rotation, flags, respawn[obj], 0))
    rows.sort(key=lambda r: (r[5], r[3], r[4], r[6], r[0]))  # rows of one mapsquare stay contiguous
    body = b"".join(struct.pack("<IQiHHHBBBBBBHH", *r) for r in rows)
    out.write_bytes(struct.pack("<III", GNOD_MAGIC, 2, len(rows)) + body)
    return len(rows)
