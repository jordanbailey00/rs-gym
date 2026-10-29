# modes

A gym mode is a named game configuration over the shared engine: its map, content whitelist and
overrides, economy and encounter parameters. Each mode builds into its own `data/modes/<mode>/`.

| Mode | Map | What it is |
|---|---|---|
| `compact_osrs_v1` | `maps/compact_v1.toml` | the v1 benchmark (`SPEC.md`, "v1") |
| `island` | `maps/island.toml` | the first v1 draft: island, KBD, Chaos Elemental, wilderness |

```bash
python3 tools/build_mode.py                  # compact_osrs_v1
python3 tools/build_mode.py island
./build/cmake/rs-gym-validate compact_osrs_v1
```

## Files

`modes/<mode>/` holds the mode's configuration, names as in the b237 cache:

- `mode.toml`: name, map, player range, level cap and bands, XP multiplier, enabled skills, the
  starter kit, death, loot and respawn timing, benchmark horizon.
- `content.toml`: pins for ambiguous names, equipment use requirements by band, bone weapons,
  tools, gathering/fishing/cooking rules (mode level and the canonical one), stations, the enabled
  recipes (canonical `recipes.bin` rows selected by output and inputs, or authored rows with their
  source), spells, prayers, bones, and the ordinary enemies with their closed drop tables.
- `economy.toml`: fixed-price shops, the tanner and Historian Aldo, sale values.
- `scurrius.toml`: the boss's tuning, arena tiles and loot.
- `manifest.json` (generated): every selected item, NPC, loc, recipe and service with its id, roles,
  sources, consumers and overrides.
- `override-ledger.json` (generated): every inherited value or rule the mode replaces, from what to
  what, and why.

A mode without `content.toml` (the island) uses the hard-coded tables in `rc-content/skilling.c`
and `gym.c`.

## Pipeline

1. `tools/modec` resolves names against the cache, selects recipe rows, checks the content is
   closed (every input supplied, every station placed, every reachable item valued) and writes
   `data/modes/<mode>/mode.rules` and `manifest.json`.
2. `rc-core/mode.c` loads `mode.rules` with the game data (`RcWorldConfig.mode_rules_path`), patches
   the definitions (equipment requirements, NPC profiles, spells, prayers, the closed recipe table)
   and logs each change; the rules stay active for content and the engine's mode extensions.
3. `rc-content/compact.c` installs the mode into each world: skilling rules and stations, shops,
   services, burying, drop tables, death and loot timing, and the fresh-character kit.
4. `rs-gym-validate` loads the mode like rc-env does, checks ids, requirement caps, quest items,
   bow/arrow bands, placed stations, nodes and NPCs, and writes the ledger and
   `build/reports/<mode>-validation.txt`.

`mode.rules` is one record per line, `kind key=value ...`; lists are comma-separated and tuples
colon-separated (`inputs=436:1,438:1`).

## Implementation map (compact_osrs_v1)

| Spec area | Where |
|---|---|
| Mode registration, per-mode data | `tools/build_mode.py`, `rc-core/layout.c`, `RcEnvConfig.mode` |
| Manifest, overrides, validator | `tools/modec`, `rc-core/mode.c`, `rc-env/tools/validate.c` |
| Level cap, XP multiplier, enabled skills | `rc_add_xp_hundredths` (`rc-core/skills.c`) |
| Rebased gathering, fishing, cooking curves | `level_offset` in `rc-core/skilling.h` rules |
| Production stations (furnace, anvil, knife, combine, spinning, needle, altars, chisel, tanner, Aldo) | `RcMakeMenu`, `rc-core/skilling.c`, `rc-content/compact.c` |
| Fixed-price shops and sale values | `rc-core/shops.c` (`RcShopDef.fixed`) |
| Enemy profiles, closed drop tables | `rc_mode_patch_npcs`, `mode_loot` in `compact.c` |
| Starter kit | `rc_content_compact_new_character` |
| Map | `maps/compact_v1.toml` |
