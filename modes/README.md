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
- `combat_visuals.tsv` (optional): presentation rows over `data/defs/combat_visuals.tsv`, same
  columns; the viewer reads them first. NPC rows are keyed by the action names content records
  (Scurrius' attacks and mechanics) and by `Spawn`, `Death` and stand poses. Their spot animations
  also select the mode's skeletal export.
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
| Scurrius encounter | `rc-content/encounters/scurrius.c`, `modes/compact_osrs_v1/scurrius.toml` |
| Scurrius presentation | `modes/compact_osrs_v1/combat_visuals.tsv`, skeletal export (`tools/cache_pipeline/export_skeletal.py`, `rc-viewer/skeletal.h`) |
| Trading, contention, loot ownership | `rc-core/trade.c`, rotating player order in `rc-core/tick.c`, `rc-core/drops.c` |
| RL interface (compact-v2) | `rc-env/compact_env.h`, `rc-env/puffer/compact/` |
| Acceptance runs | `rc-env/tools/acceptance.c` (`rs-gym-acceptance`) |

## Acceptance (compact_osrs_v1)

```bash
./build/cmake/rs-gym-acceptance all --seeds 5 --report modes/compact_osrs_v1/acceptance-report.md
./build/cmake/rs-gym-acceptance routes --seeds 1 --style magic     # one run; logs in build/acceptance/
```

Scripted controllers (`rc-env/script.h`) play everything through player commands; they know the
reference map but never set levels, grant items, teleport or read the RNG. Sections:

- `routes`: a fresh character per style (melee, ranged, magic) and seed plays to a credited
  Scurrius kill inside the 12,000-tick horizon: cows (beef cooked for food), goblins, hill giants,
  town trips for gear, food, arrows and runes, grave recovery, then the boss with prayer flicked on
  its attack timer and rats cleared while it feeds. Also reports activity throughput.
- `coop`: a melee fighter and a mage, a cook and a runecrafter who sell them food and runes through
  trades; the fighters kill Scurrius together.
- `contention`: eight players on shared rocks, one shop stock, one dropped item and the boss.
- `economy`: buy/sell-back and dose arbitrage, conservation through ordinary actions, death from
  zero cash twice with the grave among aggressive monsters, arrow production from the kit.
- `perf`: compact-v2 ticks per second for 1, 4 and 8 agents and for parallel worlds.

The report (`modes/compact_osrs_v1/acceptance-report.md`) lists every run with its seed and end-state
hash; rerunning the command reproduces them exactly. ctest runs the same sections at two seeds.
Tuning changes found by these runs are noted where the value lives (`scurrius.toml`) and in the
override ledger.
