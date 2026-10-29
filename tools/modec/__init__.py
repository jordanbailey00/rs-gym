"""Mode compiler: turns a mode's TOML tables (names, as in the b237 cache) into the records the engine
loads (rc-core/mode.c) and the content manifest.

    rules = compile_mode(mode_dir, out_dir, placements)

It resolves every item, NPC and loc against the cache, selects the canonical recipes.bin rows the
mode enables, checks that the content is closed (every input obtainable, every station placed, every
reachable item valued), and writes `<out>/mode.rules` and `<mode_dir>/manifest.json`. The returned
object carries the gathering-node settings and drop tables the build writes next.
"""

from __future__ import annotations

import json
import struct
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "cache_pipeline"))

from rc_cache import RcCacheStore  # noqa: E402
from rc_cache.definitions import (  # noqa: E402
    decode_item_definition, decode_location_definition, decode_npc_definition)

SOURCE_CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
CANONICAL_RECIPES = ROOT / "data/defs/recipes.bin"

SKILLS = ["Attack", "Defence", "Strength", "Hitpoints", "Ranged", "Prayer", "Magic", "Cooking",
          "Woodcutting", "Fletching", "Fishing", "Firemaking", "Crafting", "Smithing", "Mining",
          "Herblore", "Agility", "Thieving", "Slayer", "Farming", "Runecraft", "Hunter",
          "Construction"]
SKILL = {name: i for i, name in enumerate(SKILLS)}

# Production menus (rc-core RcMakeMenu) by station name.
STATIONS = {"Furnace": "furnace", "Anvil": "anvil", "Knife": "knife", "Hands": "combine",
            "Spinning wheel": "spin", "Needle": "needle", "Altar": "altar", "Chisel": "chisel"}

OBJ_SKILL_OTHER = 0  # gathering-node behaviour skill for nodes outside the object behaviours


class ModeError(SystemExit):
    pass


@dataclass
class Rules:
    node_respawn: dict[int, int] = field(default_factory=dict)
    node_depleted: dict[int, int] = field(default_factory=dict)
    extra_nodes: dict[int, int] = field(default_factory=dict)  # loc -> node skill, action option
    drops: dict = field(default_factory=dict)                   # DROP v2 tables (closed)


# ---- Cache names --------------------------------------------------------------------------------

class Names:
    """Item, NPC and loc names from the cache, with the mode's pins for ambiguous names."""

    def __init__(self, pins: dict[str, int]):
        store = RcCacheStore(SOURCE_CACHE)
        self.items, self.npcs, self.locs = {}, {}, {}
        for group, table, decode in ((10, self.items, decode_item_definition),
                                     (9, self.npcs, decode_npc_definition),
                                     (6, self.locs, decode_location_definition)):
            for i, data in store.read_group(2, group).items():
                try:
                    table[i] = decode(i, data)
                except Exception:  # noqa: BLE001 - undecodable rows are simply not selectable
                    pass
        self.by_name: dict[str, list[int]] = {}
        for i, d in sorted(self.items.items()):
            if (d.note_template_id or -1) >= 0 or (d.placeholder_template_id or -1) >= 0:
                continue
            self.by_name.setdefault((d.name or "").lower(), []).append(i)
        self.pins = {k.lower(): v for k, v in pins.items()}
        self.ambiguous: dict[str, list[int]] = {}

    def item(self, ref) -> int:
        if isinstance(ref, int):
            if ref not in self.items:
                raise ModeError(f"unknown item id {ref}")
            return ref
        key = ref.lower()
        if key in self.pins:
            pinned = self.pins[key]
            if pinned not in self.items or (self.items[pinned].name or "").lower() != key:
                raise ModeError(f"[ids] pins {ref!r} to {pinned}, which is not that item")
            return pinned
        ids = self.by_name.get(key, [])
        if not ids:
            raise ModeError(f"no item is named {ref!r}")
        if len(ids) > 1:
            self.ambiguous[ref] = ids  # reported together once the whole mode is read
        return ids[0]

    def item_name(self, item: int) -> str:
        return self.items[item].name

    def npc(self, npc_id: int, name: str | None = None) -> int:
        d = self.npcs.get(npc_id)
        if not d or (name and d.name.lower() != name.lower()):
            raise ModeError(f"npc {npc_id} is {d.name if d else 'unknown'}, not {name}")
        return npc_id

    def loc(self, loc_id: int) -> int:
        if loc_id not in self.locs:
            raise ModeError(f"unknown loc id {loc_id}")
        return loc_id


# ---- Canonical recipes --------------------------------------------------------------------------

@dataclass
class Recipe:
    row: int            # canonical recipes.bin row, -1 for mode-authored rows
    name: str
    reqs: list          # (skill, level, xp tenths)
    inputs: list        # (item, quantity)
    tools: list
    facility: str
    output: int
    qty: int
    ticks: int
    flags: int


def read_recipes(path: Path) -> list[Recipe]:
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<III", data, 0)
    pos = 12
    rows = []

    def text() -> str:
        nonlocal pos
        n = data[pos]
        s = data[pos + 1:pos + 1 + n].decode("latin1")
        pos += 1 + n
        return s

    for i in range(count):
        name = text()
        reqs, inputs, tools = [], [], []
        for _ in range(data[(pos := pos + 1) - 1]):
            reqs.append(struct.unpack_from("<BBH", data, pos))
            pos += 4
        for _ in range(data[(pos := pos + 1) - 1]):
            inputs.append(struct.unpack_from("<IH", data, pos))
            pos += 6
        for _ in range(data[(pos := pos + 1) - 1]):
            tools.append(struct.unpack_from("<I", data, pos)[0])
            pos += 4
        facility = text()
        output, qty, ticks, flags = struct.unpack_from("<IHHB", data, pos)
        pos += 9
        rows.append(Recipe(i, name, reqs, inputs, tools, facility, output, qty, ticks, flags))
    if pos != len(data):
        raise ModeError(f"{path}: trailing recipe data")
    return rows


# ---- Placements -----------------------------------------------------------------------------------

def placed_locs(placements: Path) -> dict[int, int]:
    """loc id -> count of placements in the map (OPLI v1 index)."""
    data = placements.read_bytes()
    magic, version, rows, record = struct.unpack_from("<4I", data, 0)
    base = 36 + 65536 * 8
    counts: dict[int, int] = {}
    for row in struct.iter_unpack("<IQHHHBBBB", data[base:base + rows * record]):
        counts[row[0]] = counts.get(row[0], 0) + 1
    return counts


# ---- Compiler -------------------------------------------------------------------------------------

def _q(text: str) -> str:
    return '"' + text.replace('"', "'") + '"'


def _tenths(xp: float) -> int:
    return int(round(xp * 10))


class Compiler:
    def __init__(self, mode_dir: Path, placements: Path | None):
        self.dir = mode_dir
        self.mode = tomllib.loads((mode_dir / "mode.toml").read_text())
        self.content = tomllib.loads((mode_dir / "content.toml").read_text())
        econ = mode_dir / "economy.toml"
        self.economy = tomllib.loads(econ.read_text()) if econ.is_file() else {}
        boss = mode_dir / "scurrius.toml"
        self.boss = tomllib.loads(boss.read_text()) if boss.is_file() else {}
        self.names = Names(self.content.get("ids", {}))
        self.recipes = read_recipes(CANONICAL_RECIPES)
        self.placed = placed_locs(placements) if placements else None
        self.cap = self.mode["mode"]["level_cap"]
        self.lines: list[str] = []
        self.manifest: dict[str, dict] = {"items": {}, "npcs": {}, "locs": {}, "recipes": {}, "services": {}}
        self.obtainable: dict[int, set[str]] = {}   # item -> how it enters the world
        self.needed: dict[int, set[str]] = {}       # item -> what consumes or needs it
        self.values: dict[int, int] = {}            # item -> sale value
        self.rules = Rules()

    # ---- bookkeeping

    def emit(self, record: str, **fields) -> None:
        parts = [record]
        for k, v in fields.items():
            if isinstance(v, bool):
                v = int(v)
            if isinstance(v, (list, tuple)):
                v = ",".join(":".join(map(str, x)) if isinstance(x, (list, tuple)) else str(x) for x in v)
            parts.append(f"{k}={v}")
        self.lines.append(" ".join(parts))

    def item(self, ref, role: str | None = None) -> int:
        i = self.names.item(ref)
        entry = self.manifest["items"].setdefault(i, {
            "id": i, "name": self.names.item_name(i), "roles": [], "sources": [], "needed_by": [],
            "overrides": [], "tradeable": bool(self.names.items[i].tradeable),
            "members": bool(self.names.items[i].members)})
        if role and role not in entry["roles"]:
            entry["roles"].append(role)
        return i

    def source(self, item: int, how: str) -> None:
        self.item(item)
        self.obtainable.setdefault(item, set()).add(how)
        sources = self.manifest["items"][item]["sources"]
        if how not in sources:
            sources.append(how)

    def need(self, item: int, by: str) -> None:
        self.item(item)
        self.needed.setdefault(item, set()).add(by)
        needed = self.manifest["items"][item]["needed_by"]
        if by not in needed:
            needed.append(by)

    def override(self, kind: str, key: int, what: str) -> None:
        table = self.manifest["items" if kind == "item" else kind]
        if key in table and what not in table[key]["overrides"]:
            table[key]["overrides"].append(what)

    def npc_entry(self, npc: int, role: str) -> None:
        d = self.names.npcs[npc]
        e = self.manifest["npcs"].setdefault(npc, {"id": npc, "name": d.name, "roles": [], "overrides": [],
                                                   "combat_level": d.combat_level})
        if role not in e["roles"]:
            e["roles"].append(role)

    def loc_entry(self, loc: int, role: str) -> None:
        d = self.names.locs[loc]
        e = self.manifest["locs"].setdefault(loc, {"id": loc, "name": d.name, "roles": [],
                                                   "placed": None})
        if role not in e["roles"]:
            e["roles"].append(role)
        if self.placed is not None:
            e["placed"] = self.placed.get(loc, 0)

    def check_level(self, what: str, level: int) -> None:
        if not 1 <= level <= self.cap:
            raise ModeError(f"{what}: requirement {level} is outside 1..{self.cap}")

    # ---- sections

    def mode_records(self) -> None:
        m = self.mode["mode"]
        mask = 0
        for s in m["skills"]:
            mask |= 1 << SKILL[s]
        self.emit("mode", name=m["name"], version=m["version"], cap=self.cap, xp_mult=m["xp_multiplier"],
                  max_players=m["max_players"], members=m["members"],
                  horizon=self.mode.get("benchmark", {}).get("horizon", 0))
        self.emit("skills", mask=mask)
        bands = m["bands"]
        if sorted(bands) != bands or bands[-1] > self.cap:
            raise ModeError(f"bands {bands} must rise and stay within the cap")
        d, loot = self.mode["death"], self.mode["loot"]
        self.emit("death", respawn=d["respawn_ticks"], recovery=d["recovery_ticks"])
        self.emit("loot", private=loot["private_ticks"], public=loot["public_ticks"])
        self.emit("npcrespawn", ticks=self.mode["resources"]["npc_respawn"])
        for where, key in (("equip", "equipment"), ("inv", "inventory"), ("bank", "bank")):
            for name, qty in self.mode["start"][key]:
                i = self.item(name, "starter kit")
                self.source(i, "starter kit")
                self.emit("start", where=where, item=i, qty=qty)

    def equipment(self) -> None:
        bands = {}
        for group in self.content["equip"]:
            band = group["band"]
            for skill in ("attack", "defence", "ranged", "magic"):
                for name in group.get(skill, []):
                    i = self.item(name, "equipment")
                    reqs = bands.setdefault(i, {})
                    reqs[SKILL[skill.capitalize()]] = band
        tool_skill = {"axe": SKILL["Woodcutting"], "pickaxe": SKILL["Mining"]}
        for tool in self.content["tool"]:
            for name, level, value in tool["items"]:
                i = self.item(name, f"{tool['kind']}")
                self.check_level(name, level)
                field_name = "scale" if tool["kind"] == "axe" else "ticks"
                self.emit("tool", kind=tool["kind"], item=i, level=level, **{field_name: value})
                self.need(i, f"{SKILLS[tool_skill[tool['kind']]]} tool")
        for i, reqs in sorted(bands.items()):
            for skill, level in reqs.items():
                self.check_level(self.names.item_name(i), level)
            self.emit("equip", item=i, levels=sorted(reqs.items()))
            self.override("item", i, "use requirements")
        for bw in self.content.get("bone_weapon", []):
            i = self.item(bw["item"], "bone weapon")
            src = self.item(bw["bonuses_from"])
            self.emit("bonus", item=i, base=src)
            self.override("item", i, "combat bonuses")
            fields = {"item": i, "rat_bonus": bw["rat_max_hit"]}
            if "powered_max_hit" in bw:
                fields.update(powered_max=bw["powered_max_hit"], speed=bw["attack_speed"],
                              charge=self.item(bw["charge"], "staff charge"))
                self.override("item", i, "powered spell")
            fields["rats"] = self._rat_ids()
            self.emit("boneweapon", **fields)

    def _rat_ids(self) -> list[int]:
        rats = [n["id"] for n in self.content["npc"] if "rat" in n["name"].lower()]
        if self.boss:
            rats += [self.boss["boss"]["npc"], self.boss["boss"]["minion"]]
        return sorted(set(rats))

    def gathering(self) -> None:
        res = self.mode["resources"]
        for g in self.content["gather"]:
            product = self.item(g["product"], "gathered resource")
            skill = SKILL[g["skill"]]
            self.check_level(g["name"], g["level"])
            offset = g["canonical"] - g["level"]
            kind = g["skill"]
            if g["deplete_one_in"] == 0:
                respawn = 0
            elif g.get("no_tool"):
                respawn = res["flax_respawn"]
            elif kind == "Woodcutting":
                respawn = res["tree_respawn"]
            else:
                respawn = res["rock_respawn"]
            for loc in g["locs"]:
                self.names.loc(loc)
                self.loc_entry(loc, f"{kind.lower()} node")
                self.emit("gather", loc=loc, product=product, skill=skill, level=g["level"], offset=offset,
                          xp=_tenths(g["xp"]), low=g["low"] * 10, high=g["high"] * 10,
                          deplete=g["deplete_one_in"], respawn=respawn, notool=g.get("no_tool", False),
                          ticks=g.get("ticks", 0))
                self.rules.node_respawn[loc] = respawn
                if g.get("no_tool") or kind == "Mining" and g["deplete_one_in"] == 0:
                    self.rules.extra_nodes[loc] = OBJ_SKILL_OTHER
                if g.get("no_tool"):
                    self.rules.node_depleted[loc] = -1
            self.source(product, f"{kind.lower()} ({g['name']})")
            if offset:
                self.override("item", product, f"{kind} level {g['canonical']} -> {g['level']}")
        for f in self.content["fish"]:
            npc = self.names.npc(f["npc"])
            self.npc_entry(npc, "fishing spot")
            tool = self.item(f["tool"], "fishing tool")
            self.need(tool, "fishing")
            bait = self.item(f["bait"], "fishing bait") if f.get("bait") else 0
            if bait:
                self.need(bait, "fishing")
            catches = []
            for name, level, canonical, xp, low, high in f["catches"]:
                product = self.item(name, "raw fish")
                self.check_level(name, level)
                self.source(product, "fishing")
                catches.append((product, level, canonical - level, _tenths(xp), low * 10, high * 10))
                if canonical != level:
                    self.override("item", product, f"Fishing level {canonical} -> {level}")
            self.emit("fish", npc=npc, option=f["option"], tool=tool, bait=bait, catch=catches)

    def cooking(self) -> None:
        cook = self.content["cooking"]
        for loc in cook["ranges"]:
            self.names.loc(loc)
            self.loc_entry(loc, "range")
            self.emit("cookobj", loc=loc)
        if self.placed is not None and not any(self.placed.get(l) for l in cook["ranges"]):
            raise ModeError("no cooking range is placed in the map")
        for raw, cooked, burnt, level, canonical, xp, low, high in cook["foods"]:
            r, c, b = self.item(raw, "raw food"), self.item(cooked, "food"), self.item(burnt, "burnt food")
            self.check_level(self.names.item_name(c), level)
            self.need(r, "cooking")
            self.source(c, "cooking")
            self.source(b, "cooking (burnt)")
            self.emit("cook", raw=r, cooked=c, burnt=b, level=level, offset=canonical - level, xp=_tenths(xp),
                      low=low * 10, high=high * 10)
            if canonical != level:
                self.override("item", c, f"Cooking level {canonical} -> {level}")

    def stations(self) -> None:
        for kind, locs in self.content.get("stations", {}).items():
            for loc in locs:
                self.names.loc(loc)
                self.loc_entry(loc, kind.replace("_", " "))
                self.emit("station", kind=STATIONS[kind.replace("_", " ").capitalize()], loc=loc)
            if self.placed is not None and not any(self.placed.get(l) for l in locs):
                raise ModeError(f"no {kind} is placed in the map")

    def _find_row(self, output: int, inputs: list[int]) -> Recipe:
        want = sorted(inputs)
        rows = [r for r in self.recipes if r.output == output and sorted(i for i, _ in r.inputs) == want]
        if not rows:
            raise ModeError(f"no recipes.bin row makes {self.names.item_name(output)} from "
                            f"{[self.names.item_name(i) for i in inputs]}")
        return rows[0]

    def _recipe(self, key: str, rec: Recipe, station: str, skill: int, level: int, *, inputs=None,
                tools=None, xp=None, locs=(), batch=False, note: str = "", canonical_level=None) -> None:
        inputs = rec.inputs if inputs is None else inputs
        tools = rec.tools if tools is None else tools
        req_xp = xp if xp is not None else next((x for s, _, x in rec.reqs if s == skill), 0)
        reqs = [(skill, level, req_xp)] if skill >= 0 else []
        idx = len(self.manifest["recipes"])
        for i, _ in inputs:
            self.need(i, f"recipe {key}")
        for t in tools:
            self.need(t, f"recipe {key} (tool)")
        self.source(rec.output, f"recipe {key}")
        self.emit("recipe", id=idx, station=station, out=rec.output, qty=rec.qty, ticks=rec.ticks or 4,
                  canonical=rec.row, reqs=reqs, inputs=inputs, tools=tools, locs=list(locs), batch=batch)
        overrides = []
        canon = next(((s, lv) for s, lv, _ in rec.reqs if s == skill), None)
        if rec.row >= 0 and canon and canon[1] != level:
            overrides.append(f"{SKILLS[skill]} level {canon[1]} -> {level}")
        if rec.row < 0:
            overrides.append(f"authored row ({note})")
        elif inputs != rec.inputs:
            overrides.append("inputs " + ", ".join(f"{self.names.item_name(i)} x{q}" for i, q in rec.inputs)
                             + " -> " + ", ".join(f"{self.names.item_name(i)} x{q}" for i, q in inputs))
        if rec.row >= 0 and list(tools) != list(rec.tools):
            overrides.append("tools " + (", ".join(self.names.item_name(t) for t in rec.tools) or "none")
                             + " -> " + (", ".join(self.names.item_name(t) for t in tools) or "none"))
        if rec.row >= 0 and len(rec.reqs) > 1 and skill >= 0:
            overrides.append("other requirements dropped")
        self.manifest["recipes"][idx] = {
            "id": idx, "key": key, "output": rec.output, "output_name": self.names.item_name(rec.output),
            "quantity": rec.qty, "station": station, "canonical_row": rec.row,
            "requirements": [{"skill": SKILLS[s], "level": lv, "xp": x / 10} for s, lv, x in reqs],
            "inputs": [{"item": i, "name": self.names.item_name(i), "quantity": q} for i, q in inputs],
            "tools": [{"item": t, "name": self.names.item_name(t)} for t in tools],
            "ticks": rec.ticks, "overrides": overrides, "note": note}
        if canonical_level is not None and canonical_level != level:
            self.manifest["recipes"][idx]["overrides"].append(f"{SKILLS[skill]} level {canonical_level} -> {level}")

    def production(self) -> None:
        tool_of = {"Knife": "Knife", "Needle": "Needle", "Chisel": "Chisel"}
        for group in self.content["recipe"]:
            station = STATIONS[group["station"]]
            skill = SKILL[group["skill"]]
            level = group["level"]
            self.check_level(f"{group['skill']} recipes", level)
            altars = group.get("altars", [])
            for n, (out_name, in_names) in enumerate(group["rows"]):
                out = self.item(out_name, f"{group['skill'].lower()} product")
                ins = [self.item(x, "ingredient") for x in in_names]
                row = self._find_row(out, ins)
                tools = list(row.tools)
                if group["station"] == "Altar":
                    tools = []  # altars here need no talisman or tiara
                    loc = altars[n]
                    self.names.loc(loc)
                    self.loc_entry(loc, f"{out_name.lower()} altar")
                    if self.placed is not None and not self.placed.get(loc):
                        raise ModeError(f"the {out_name} altar (loc {loc}) is not placed in the map")
                for t in tools:
                    self.item(t, "tool")
                if group["station"] in tool_of:
                    t = self.item(tool_of[group["station"]], "tool")
                    if t not in tools:
                        tools.append(t)
                self._recipe(out_name, row, station, skill, level, tools=tools,
                             locs=[altars[n]] if altars else (), batch=group["station"] == "Altar")
        for a in self.content.get("authored", []):
            out = self.item(a["output"], f"{a['skill'].lower()} product")
            inputs = [(self.item(n, "ingredient"), q) for n, q in a["inputs"]]
            tools = [self.item(t, "tool") for t in a.get("tools", [])]
            rec = Recipe(-1, a["output"], [], inputs, tools, a["station"], out, a.get("quantity", 1),
                         a["ticks"], 0)
            self.check_level(a["output"], a["level"])
            self._recipe(a["output"], rec, STATIONS[a["station"]], SKILL[a["skill"]], a["level"],
                         xp=_tenths(a["xp"]), note=a["source"], canonical_level=a.get("canonical_level"))
        for b in self.content.get("bone_recipe", []):
            out = self.item(b["output"], "bone weapon")
            canon = self._find_row(out, [self.item(x) for x in b["canonical_inputs"]])
            inputs = [(self.item(n, "ingredient"), q) for n, q in b["inputs"]]
            skill = SKILL[b["skill"]]
            self.check_level(b["output"], b["level"])
            self._recipe(b["output"], canon, STATIONS[b["station"]], skill, b["level"], inputs=inputs)
            self.override("item", out, "creation recipe")

    def magic_prayer(self) -> None:
        for name, level, max_hit in self.content["magic"]["spells"]:
            self.check_level(name, level)
            self.emit("spell", name=_q(name), level=level, max=max_hit)
        for name, level in self.content["prayer"]["prayers"]:
            self.check_level(name, level)
            self.emit("prayer", name=_q(name), level=level)
        for name, xp in self.content["prayer"]["bones"]:
            i = self.item(name, "bones")
            self.need(i, "Prayer (bury)")
            self.emit("bury", item=i, xp=_tenths(xp))

    def enemies(self) -> None:
        for n in self.content["npc"]:
            npc = self.names.npc(n["id"], n["name"])
            self.npc_entry(npc, "enemy")
            fields = {"id": npc, "hp": n["hp"], "max": n["max_hit"]}
            if "defence" in n:
                fields["defence"] = n["defence"]
            if "weakness" in n:
                element, pct = n["weakness"]
                fields["weakness"] = f"{element.lower()}:{pct}"
            self.emit("npc", **fields)
            self.manifest["npcs"][npc]["overrides"] += ["hitpoints", "max hit"] + (
                ["defence bonuses"] if "defence" in n else []) + (["elemental weakness"] if "weakness" in n else [])
            self._drop_records(npc, n.get("always", []), n.get("rolls", []), f"{n['name']} drop")

    def _drop_records(self, npc: int, always, rolls, how: str) -> None:
        rows = []
        for name, lo, hi in always:
            i = self.item(name, "drop")
            self.source(i, how)
            rows.append((i, lo, hi))
        self.emit("drop", npc=npc, always=rows)
        for roll in rolls:
            pick = []
            for name, lo, hi, weight in roll["pick"]:
                i = self.item(name, "drop")
                self.source(i, how)
                pick.append((i, lo, hi, weight))
            self.emit("droproll", npc=npc, one_in=roll["one_in"], pick=pick)

    def shops(self) -> None:
        econ = self.economy
        for idx, shop in enumerate(econ.get("shop", [])):
            npc = self.names.npc(shop["npc"])
            self.npc_entry(npc, "shopkeeper")
            self.emit("shop", idx=idx, name=_q(shop["name"]), npc=npc, buys=shop.get("buys", "stock"))
            self.manifest["services"][f"shop:{shop['name']}"] = {"npc": npc, "kind": "shop", "goods": []}
            for name, price, cap, qty, ticks in shop["items"]:
                i = self.item(name, "shop good")
                self.source(i, f"shop {shop['name']}")
                self.emit("stock", shop=idx, item=i, price=price, cap=cap, qty=qty, ticks=ticks)
                self.values.setdefault(i, price * econ["sale"]["buyback_pct"] // 100)
                self.manifest["services"][f"shop:{shop['name']}"]["goods"].append(
                    {"item": i, "name": self.names.item_name(i), "price": price, "stock": cap,
                     "restock": [qty, ticks]})
        sale = econ.get("sale", {})
        for name, value in sale.get("values", []):
            self.values[self.item(name)] = value
        for name in sale.get("zero", []):
            self.values[self.item(name)] = 0
        for name, value in sale.get("purchase_values", []):
            self.emit("purchase", item=self.item(name), value=value)
        for i, value in sorted(self.values.items()):
            self.emit("sale", item=i, value=value)
            if i in self.manifest["items"]:
                self.manifest["items"][i]["sale_value"] = value
        tan = econ.get("tanner")
        if tan:
            npc = self.names.npc(tan["npc"])
            self.npc_entry(npc, "tanner")
            coins = self.item("Coins", "currency")
            self.emit("service", kind="tanner", npc=npc)
            for hide, leather, fee in tan["rows"]:
                h, l = self.item(hide, "hide"), self.item(leather, "tanned leather")
                rec = Recipe(-1, leather, [], [(h, 1), (coins, fee)], [], "Tanner", l, 1, 1, 0)
                self._recipe(f"tan {leather}", rec, "tanner", -1, 0, note=f"Ellis tans for {fee} coins")
            self.manifest["services"]["tanner"] = {"npc": npc, "kind": "tanner", "rows": tan["rows"]}
        aldo = econ.get("aldo")
        if aldo:
            npc = self.names.npc(aldo["npc"])
            self.npc_entry(npc, "bone-weapon service")
            coins = self.item("Coins", "currency")
            self.emit("service", kind="aldo", npc=npc)
            for b in self.content.get("bone_recipe", []):
                out = self.item(b["output"])
                inputs = [(self.item(n), q) for n, q in b["inputs"]] + [(coins, aldo["fee"])]
                rec = Recipe(-1, b["output"], [], inputs, [], "Aldo", out, 1, 4, 0)
                self._recipe(f"Aldo {b['output']}", rec, "aldo", -1, 0,
                             note=f"paid service: {aldo['fee']} coins, no creation requirement or XP")
            self.manifest["services"]["aldo"] = {"npc": npc, "kind": "bone weapons", "fee": aldo["fee"]}

    def scurrius(self) -> None:
        if not self.boss:
            return
        b = self.boss["boss"]
        boss = self.names.npc(b["npc"], "Scurrius")
        minion = self.names.npc(b["minion"], "Giant rat")
        self.npc_entry(boss, "boss")
        self.npc_entry(minion, "boss minion")
        fields = {k: v for k, v in b.items() if k not in ("npc", "minion", "defence_bonus")}
        self.emit("boss", npc=boss, minion=minion, defence_bonus=b["defence_bonus"], **fields)
        a = self.boss["arena"]
        self.emit("arena", rect=a["rect"], centre=a["centre"], spawn=a["spawn"], food=a["food_piles"])
        loot = self.boss["loot"]
        always = []
        for name, lo, hi in loot["always"]:
            i = self.item(name, "boss loot")
            self.source(i, "Scurrius")
            always.append((i, lo, hi))
        bundle = []
        for name, qty in loot["bundle"]:
            i = self.item(name, "boss loot")
            self.source(i, "Scurrius")
            bundle.append((i, qty))
        extra = []
        for name, qty, one_in in loot["extra"]:
            i = self.item(name, "boss loot")
            self.source(i, "Scurrius")
            extra.append((i, qty, one_in))
        self.emit("bossloot", always=always, bundle=bundle, extra=extra)
        self.manifest["npcs"][boss]["overrides"] += ["hitpoints", "max hit", "levels", "defence bonuses",
                                                     "group scaling", "loot"]
        self.manifest["npcs"][minion]["overrides"] += ["hitpoints", "max hit"]

    # ---- closure

    def check_closed(self) -> None:
        problems = []
        for item, users in sorted(self.needed.items()):
            if item not in self.obtainable:
                problems.append(f"{self.names.item_name(item)} ({item}) is needed by {sorted(users)} "
                                "but nothing supplies it")
        for item, how in sorted(self.obtainable.items()):
            if item == self.names.item("Coins"):
                continue
            if item not in self.values:
                problems.append(f"{self.names.item_name(item)} ({item}) enters the world ({sorted(how)}) "
                                "without a sale value in economy.toml")
        if problems:
            raise ModeError("mode content is not closed:\n  " + "\n  ".join(problems))

    def compile(self, out: Path) -> Rules:
        self.mode_records()
        self.equipment()
        self.gathering()
        self.cooking()
        self.stations()
        self.production()
        self.magic_prayer()
        self.enemies()
        self.shops()
        self.scurrius()
        if self.names.ambiguous:
            raise ModeError("pin these item names in content.toml [ids]:\n  " + "\n  ".join(
                f"{n!r}: {ids[:12]}" for n, ids in sorted(self.names.ambiguous.items())))
        self.check_closed()
        name = self.mode["mode"]["name"]
        header = (f"# {name} mode rules (rc-core/mode.h), written by tools/modec from modes/{name}/*.toml.\n"
                  "# One record per line: kind key=value ...; lists are comma-separated, tuples colon-separated.\n")
        (out / "mode.rules").write_text(header + "\n".join(self.lines) + "\n")
        manifest = {
            "format": "rs-gym-content-manifest-v1",
            "mode": name, "version": self.mode["mode"]["version"],
            "sources": {"cache": SOURCE_CACHE.parent.name, "recipes": str(CANONICAL_RECIPES.relative_to(ROOT)),
                        "mode_tables": [f"modes/{name}/{f}" for f in ("mode.toml", "content.toml",
                                                                       "economy.toml", "scurrius.toml")]},
            "items": [self.manifest["items"][k] for k in sorted(self.manifest["items"])],
            "npcs": [self.manifest["npcs"][k] for k in sorted(self.manifest["npcs"])],
            "locs": [self.manifest["locs"][k] for k in sorted(self.manifest["locs"])],
            "recipes": [self.manifest["recipes"][k] for k in sorted(self.manifest["recipes"])],
            "services": self.manifest["services"],
        }
        (self.dir / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
        return self.rules


def compile_mode(mode_dir: Path, out: Path, placements: Path | None = None) -> Rules:
    return Compiler(mode_dir, placements).compile(out)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="compile a mode's rules (dry run without placements)")
    ap.add_argument("mode")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    mode_dir = ROOT / "modes" / args.mode
    out = args.out or ROOT / "build" / "modes" / args.mode
    out.mkdir(parents=True, exist_ok=True)
    placements = ROOT / "data" / "modes" / args.mode / "regions/world.object-placements.indexed.bin"
    rules = compile_mode(mode_dir, out, placements if placements.is_file() else None)
    print(f"wrote {out / 'mode.rules'}: {len(rules.node_respawn)} node locs")
