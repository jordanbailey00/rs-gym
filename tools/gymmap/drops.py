"""NPC drop tables (DROP v2): the source table with gym-authored tables merged in.

RuneC's release strips tables whose wiki rates are unpublished (goblins, men, skeletons ...), so
those NPCs drop nothing. The gym spec's [[drops]] tables replace them with OSRS-like tables.
"""

from __future__ import annotations

import struct
from pathlib import Path

DROP_MAGIC = 0x504F5244
DROP_VERSION = 2
ALWAYS, MAIN, TERTIARY = 0, 1, 2


def read_drops(path: Path) -> dict[int, tuple[int, list, list, list, int]]:
    """npc id -> (rejection flags, always rows, main rows, tertiary rows, rare table weight)."""
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<III", data, 0)
    if magic != DROP_MAGIC or version != DROP_VERSION:
        raise SystemExit(f"{path}: not a DROP v2 table")
    pos, tables = 12, {}
    for _ in range(count):
        npc, flags = struct.unpack_from("<II", data, pos)
        pos += 8
        kinds = []
        for kind in (ALWAYS, MAIN, TERTIARY):
            n = data[pos]
            pos += 1
            rows = []
            for _ in range(n):
                item, qmin, qmax = struct.unpack_from("<IHH", data, pos)
                pos += 8
                rarity = 1
                if kind != ALWAYS:
                    (rarity,) = struct.unpack_from("<I", data, pos)
                    pos += 4
                rows.append((item, qmin, qmax, rarity))
            kinds.append(rows)
        (rare,) = struct.unpack_from("<I", data, pos)
        pos += 4
        tables[npc] = (flags, *kinds, rare)
    if pos != len(data):
        raise SystemExit(f"{path}: trailing drop data")
    return tables


def write_drops(path: Path, tables: dict) -> None:
    out = bytearray(struct.pack("<III", DROP_MAGIC, DROP_VERSION, len(tables)))
    for npc in sorted(tables):
        flags, always, main, tertiary, rare = tables[npc]
        out += struct.pack("<II", npc, flags)
        for kind, rows in ((ALWAYS, always), (MAIN, main), (TERTIARY, tertiary)):
            if len(rows) > 255:
                raise SystemExit(f"npc {npc}: more than 255 drop rows")
            out.append(len(rows))
            for item, qmin, qmax, rarity in rows:
                out += struct.pack("<IHH", item, qmin, qmax)
                if kind != ALWAYS:
                    out += struct.pack("<I", rarity)
        out += struct.pack("<I", rare)
    path.write_bytes(bytes(out))


def merge_gym_drops(source: Path, out: Path, specs: list) -> int:
    """Write the source tables with each spec table replacing its NPCs' tables; returns NPCs set."""
    tables = read_drops(source)
    changed = 0
    for spec in specs:
        rows = lambda key: [tuple(r) if len(r) == 4 else (*r, 1) for r in spec.get(key, [])]  # noqa: E731
        always = [r[:3] + (1,) for r in rows("always")]
        for npc in spec["npcs"]:
            tables[npc] = (0, always, rows("main"), rows("tertiary"), 0)
            changed += 1
    write_drops(out, tables)
    return changed
