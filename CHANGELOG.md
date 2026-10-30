# Changelog

## 2026-10-30: Scurrius moves, attacks and fights like the boss it is

**What.**
- Skeletal animation. Scurrius' models and sequences are bone-animated ("maya"), which the viewer
  could not play, so it stood frozen. `tools/cache_pipeline/rc_cache/skeletal.py` decodes skeletons,
  keyframe curves and per-vertex bone weights; `export_skeletal.py` (a new `build_mode.py` step)
  bakes per-tick skin matrices for each skeletal NPC and spot animation a mode uses into
  `data/modes/<mode>/models/skeletal.bin`. `rc-viewer/skeletal.h` registers those sequences with the
  animation cache and skins the models, so its idle, walk, run, attacks, eating, spawn and death play.
- Presentation. `modes/compact_osrs_v1/combat_visuals.tsv` (copied into the mode's data and read
  ahead of the shared table) gives each named action its sequence and effects: two tail swipes,
  flying fur and its impact, bolts of electricity with the cast glow, bolt and sparks, eating
  variants, the jump, falling debris timed to land on the hit (new `impact_gfx_lead` column), the
  screech, the roar in the centre, the eating stand, spawn and death. Two new content hooks name
  ordinary NPC attacks (`npc_attack_key`) and stand poses (`npc_stand_key`) for presentation.
- Mechanics (`rc-content/encounters/scurrius.c`). Falling bricks come from a jump on the spot and
  rats from a screech; each takes the place of an attack, on the boss's turn and not while it walks.
  It roars on reaching the centre in the final phase. Twelve rat waves at most per fight. Its body
  stays five ticks so the death plays out.
- Viewer. Summoned giant rats are drawn (their model was never preloaded). For watching fights: a
  dev Boss teleport (`RUNEC_DEV_TRANSPORT_DEST=boss`, `RUNEC_DEV_BOSS_HP_PCT`) and
  `RC_VIEWER_SCREENSHOT_EVERY`.

**Why.** The boss did not move and showed only plain attacks; its animations, projectiles and
mechanics were missing.

**Checked.** The baked matrices equal the decoder's for all 21 sequences, and the shared models
hold the cache's vertices in the same order. Screenshots of dev fights at full, 25% and 2%
hitpoints show each action, the rats, the death and the respawn. The boss test now checks every
named action, the wave cap and the death hold; ctest and the map tests pass. Acceptance (five
seeds): 15 of 15 credited kills, the co-op kill and every contention and economy check; fights took
471-770 ticks (537-808 before).

## 2026-10-30: square wall corners block only their diagonal

**What.** Loc type 3 (a square wall corner, such as a pillar at a corridor's mouth) now marks the
corner flags the client uses, in the collision export (`tools/cache_pipeline/export_collision_map.py`),
the engine's runtime door and wall changes (`rc-core/object_runtime.c`) and interaction reach
(`rc-core/tick.c`); both had treated it as a straight wall. The two pillars dropped from the sewers
stamp are back.

**Why.** As straight walls the corners closed passages the game leaves open; the corridor to
Scurrius' broken bars was one.

**Checked.** The dungeon's walkable sections are unchanged with the pillars restored; ctest and the
map tests pass; acceptance (five seeds) still has 15 of 15 credited kills and every co-op,
contention and economy check passing.

## 2026-10-29: one island and one dungeon (visual pass)

**What.** `maps/compact_v1.toml` is rebuilt from real b237 areas laid out as one place: Rimmington's
houses round the well with the Draynor bank and the Lumbridge church, general store and smithy on
the square; Fred's farm and a fenced cow pasture to the south; a curved river under the Lumbridge
bridge to the woods; the Rimmington mine on the hill; a dark east with a graveyard round the
trapdoor, the dark wizards' stone circle round the chaos altar and the Catherby pier; an essence
quarry and henge-ringed altars. The dungeon is the Varrock sewers and the moss cave as one 192x64
stamp: planks join the ladder rooms to the main channel, two of the sewers' doors are two-way gates
into the north rooms, and the east trapdoor now comes down among the hill giants in the moss cave.
The map compiler gained smooth coasts, rivers and roads with the client's overlay shapes on their
edges, a grass underlay mix, ground decoration in real-world proportions, fences, floor overrides,
and stamp options (decorative swaps for gatherable look-alikes, coast clipping, blended edges,
plane limits). The viewer draws roofs and upper floors while the player is outdoors and can take
hidden-window screenshots. In the engine, a mapsquare's placements carry a by-tile index, so tile
lookups no longer scan the whole mapsquare, and the route search marks visited tiles with a
generation counter; the compact-v2 loc index skips locs without options.

**Why.** The old map looked unfinished: straight, stair-stepped roads across a bare green disk with
four buildings, and a dungeon of three separate stamps joined by pipes. Decorated land holds about
ten times the placements per mapsquare, which made every tile lookup slow (multi-agent throughput
had halved), hence the index.

**Checked.** `rs-gym-validate`: 698 checks pass; walking distances from the bank are 10-18 to town
services, 40-56 to basic resources and goblins, 71-94 to advanced resources and Scurrius. Acceptance
(five seeds): 15 of 15 routes kill Scurrius with credit; the co-op, contention and economy checks
pass; hill giants pay about 215 coins per 1,000 ticks (182 before) and dark wizards about 12 kills per 1,000
(about 3). Throughput: 38k ticks/s for one agent, about 100k world ticks/s for 16 worlds of 4 agents
(132k on the old map: random agents now try more unreachable tiles, and each failed route search
covers its whole window). Python map tests 15 of 15; ctest passes.

## 2026-10-29: balance and acceptance (Stage 6)

**What.** `rs-gym-acceptance` (`rc-env/tools/acceptance.c`) runs the mode's acceptance evidence
with scripted controllers: fresh-start routes to a credited Scurrius kill for melee, ranged and
magic over any number of seeds, a four-player co-op team (two fighters, a cook and a runecrafter
trading supplies), eight-player contention, economy and recovery checks, activity throughput and
compact-v2 performance; it writes `modes/compact_osrs_v1/acceptance-report.md`, and ctest runs it
at two seeds. Map: the dungeon grew to 128x64 with a Varrock sewers branch reached by a manhole,
which now holds the giant rats and goblins; hill giants moved to the cave's north end; a pipe
joins the branches; the chaos altar moved three tiles east. Scurrius: defence bonuses 20 -> 0 (the
cache value) and feeding heal 3 -> 2 per heal; the ledger records the encounter tuning. A full
bank no longer turns waiting recovery items public. `rs-gym-rollout --mode compact_osrs_v1`
records compact-v2 episodes for the viewer.

**Why.** Stage 6. The routes found three problems. Goblin-level characters died on nearly every
cave trip, because the cave held the basic tier in one room with skeletons, dark wizards and hill
giants that hunt within 8 tiles up to combat 40-56. With the provisional boss values only 3 of 15
routes won (13 with the defence bonuses at the cache's 0, 15 with the lower heal). And expiring
grave items waiting on a full bank became visible to everyone at the same tick.

**Checked.** Report: 15 of 15 routes kill Scurrius with credit (first kill about tick 10,200-11,900;
28 of 30 over ten seeds); the co-op team kills it at two participants (420 HP) after three supplier
trades; contention passes (fair rock shares, stock never negative, coins exact, one winner of a
ground race, scaling to 1,500 HP checked at every join); economy passes (no buy/sell-back or dose
gain, conservation through bank, equip, drop, shop and trade, recovery from zero twice with both
graves banked, arrows made from the kit). Runs are deterministic: the same command reproduces the
same end-state hashes. `tests/test_compact_boss.c` covers the full-bank expiry. ctest: 19 of 19.

## 2026-10-29: compact-v2 RL interface (Stage 5)

**What.** `rc-env/compact_env.h` is the mode's RL interface, schema `compact-v2`: per-agent
observations of own state, a 25x25 window (tiles, NPCs, locs, ground items, players, brick
shadows, incoming hits by style), open interfaces only (bank, shop, menu, trade), and optional
public-map landmark bearings; one flat masked action space (1,255 actions) with distinct Wait and
Stop; profiles free play, solo boss, development and cooperative boss with sparse +1 rewards and
termination kept apart from truncation; per-agent metrics and an event log; snapshots
(`rc-core/snapshot.c`), canonical state hashes, replays (rc-viewer plays them), and a PufferLib 5.0
binding (`rc-env/puffer/compact`, env `rs_gym_compact`; `tools/puffer_overlay.sh` links both envs).

**Why.** Stage 5 of the v1 roadmap.

**Checked.** `tests/test_compact_env.c`: finite observations and no masked actions under random
play; identical hashes for identical seeds and actions; a restored snapshot reproduces the
uninterrupted run; Wait keeps a walk and Stop ends it; unseen changes (another player's items and
bank, shop stock, the RNG) leave an agent's observation and mask unchanged; masked and
out-of-range actions fail safely; replays reproduce the final state; the solo boss profile
terminates with +1 (boss HP lowered as test setup). `tests/test_puffer_compact.c` runs the binding
against the stand-in header. PufferLib 5.0 has one done buffer, so the binding sets it at either
end and the observation and logs keep terminated and truncated apart.

## 2026-10-29: trading, loot ownership and contention (Stage 4)

**What.** Players can trade directly (`rc-core/trade.c`), NPC drop ownership breaks ties with the
seeded generator, and players act in an order that rotates every tick. The viewer has a trade
panel.

**Why.** Stage 4 of the v1 roadmap: one shared world with atomic trades, fair contention and no
permanent low-slot advantage, for up to eight players.

**Details.**
- Trades: mutual requests within 10 tiles open a trade; offered items leave the inventory for the
  trade, so they cannot be eaten, equipped, dropped or sold; each offer change bumps a shared
  revision and clears both acceptances; the exchange commits when both accept the same revision,
  after rechecking both players, the distance and inventory room, as two staged transactions.
  Moving, any other action, death (before the grave is made), separation or a failed check
  cancels it and returns both offers; untradeable items cannot be offered. Trade calls are soft
  player commands (`TRADE_REQUEST/OFFER/WITHDRAW/ACCEPT/DECLINE`).
- Turn order: input, routes and actions run for players starting at slot `tick % count`, so the
  last shop item, a ground item or a resource node goes to a different player each tick.
- Loot: the top non-overkill damage dealer owns an NPC's drop (private, then public, then gone, by
  the mode's 100/300 ticks); exact ties go to a seeded pick among the tied players.
- Viewer: T asks the nearest player to trade; the chatbox shows both offers, clicks on inventory
  items offer them, and Accept / Decline act on the current revision.

**Checked.** `tests/test_compact_trade.c`: a swap that conserves every item, revision
invalidation, stale accepts, eating and dropping during a trade, death mid-trade, a full
inventory, separation, a busy partner and an untradeable spine. `tests/test_compact_shared.c`:
eight players racing for the last hammer and for one ground item; one winner each time, rotating.
The trade panel has not been used by hand.

## 2026-10-29: Scurrius, bone weapons and death recovery (Stage 3)

**What.** Scurrius is the mode's shared boss (`rc-content/encounters/scurrius.c`), the three bone
weapons work in combat with the staff's charges, and death now leaves a private recovery stack
that moves to the bank when it expires.

**Why.** Stage 3 of the v1 roadmap: the advanced encounter with its recognizable mechanics and
the post-boss improvement loop, and the mode's mild death rules.

**Scurrius.** RuneC_v4 has a Scurrius encounter design (`content/encounters/scurrius.toml`) but
its runtime is a single-player pilot (bricks hit by radius, a fixed ring of rats, healing without
walking) and the data snapshot carries no Scurrius encounter rows, so the fight is a mode script
built on RuneC's attack and phase design and the wiki's description:
- Tail swipes in melee (weights 10:1:1 with bolts and fur balls) above 80%; at 80%, and every 50
  ticks after, it walks to a food pile and eats five times (3 HP per living player inside each),
  attacking only with magic and ranged while it eats; below 30% it moves to the centre and uses
  only magic and ranged at speed 4 instead of 5. Phases never regress.
- Falling bricks land 3 ticks after their shadows appear, on each living player's tile and on
  random tiles (twice as many, twice as often in the final phase), for up to 6 typeless damage.
  The viewer draws the shadows.
- From phase 2, screeches summon up to six giant rats every 30 ticks, at most eight alive; they
  outlive the boss, as in OSRS.
- Group scaling over a monotonic participant set: every living player who enters adds 75% of the
  base 240 HP to the maximum and the current; leaving and re-entering adds nothing.
- A kill resolves once, after the tick's deaths: players alive inside with at least 5% of the
  actual damage get kill credit, and the one bundle (150 coins, one of 3 mithril ore, 12 chaos
  runes or 40 feathers, 1-in-3 spine, 1-in-5 prayer potion) goes to the next of them in a seeded
  rotation; with nobody qualified, the top contributor owns it without credit. It respawns 100
  ticks later; with nobody inside for 50 ticks a fight resets boss, rats and bricks.
- The engine gained a mode tick (after deaths and respawns) and the boss state lives on the world.

**Bone weapons.** The mace and bow add 2 max hit against rats (Scurrius, its rats, giant rats);
the staff's powered attack has a fixed base max hit of 14 (+2 on rats) before ordinary magic damage
bonuses, attacks every 4 ticks, and spends one charge per cast. It is made with 100 charges (recipe
output state), gains one per chaos rune used on it up to 1000, and Uncharge returns them all.

**Death.** Nothing is kept: carried and worn items become private ground items at the death tile
for 1500 ticks, then move to the bank (retried each tick if the bank is full, never deleted).
Respawn after 10 ticks at the bank, with no new kit.

**Checked.** `tests/test_compact_boss.c`: a solo kill by a level-40 mithril fighter (875 ticks;
balance comes in Stage 6) sees every mechanic, gets the bundle and the respawn; scaling for two
and three players with a late join and a re-entry; the abandoned reset; the rat bonus; staff
charges through use and uncharge; a death's items banked on expiry. All 11 suites pass.

## 2026-10-29: compact_osrs_v1 playable economy (Stage 2)

**What.** Every production chain of the mode works through real player interactions, checked by
scripted controllers; elemental weakness and within-tier spell scaling apply in combat; the viewer
labels the new production menus; the reference map meets the spec's walking-distance targets.

**Why.** Stage 2 of the v1 roadmap: a character must be able to gather, process, equip, fight, earn
coins, buy supplies and return to town, and all three combat styles' supply chains must work before
the boss goes in.

**Details.**
- `rc-env/script.c`: scripted controllers. Each agent's plan is plain blocking C over player
  commands (walk, use locs and items, bank, shop, make, gather, fish, fight, pick up, bury); agents
  run as coroutines (`ucontext`) over one world, stepping in index order each tick. Plans may know
  the map but never write game state. Stage 6's acceptance runs build on it.
- `tests/test_compact_economy.c`: one fresh character makes every band-1 product starting from the
  bank (mine, smelt, smith arrowtips, chop, fletch shafts and a bow, pick flax, spin, string, kill a
  cow, tan, sew, cook, bury, net, mine essence, bind runes, sell and buy) in about 900 ticks, while
  a second character with level 30 and seeded materials (test setup) makes every band-15 and band-30
  product, all three bone weapons, and uses Historian Aldo's service (100 coins, no XP). XP gains
  match the ×8 multiplier.
- Combat: within-tier elemental scaling reads the (patched) spell table instead of a fixed table,
  so the mode's Strike/Bolt/Blast stay at 4/10/14; a spell matching an NPC's elemental weakness gains
  the weakness percent in accuracy and that share of its base max hit.
- Loc handlers register on the cache action slot that carries their label (flax's Pick is slot 1).
- Map: the town sits on the west side around the bank's north door (store, wheel, range, church,
  smithy, Ellis within 11-22 ticks); basic resources are 28-53 ticks away; the outer area, lobster
  coast, chaos and death altars 74-84; Scurrius 97 through the dungeon, whose pipe moved to the
  cave's far end. `rs-gym-validate` measures these paths (breadth-first, across links) and reports
  any outside the targets. Closed doors were removed from the town stamps; the house stamp gave way
  to a standalone range. Landmarks allow 32.

## 2026-10-29: compact_osrs_v1 mode, rules pipeline and reference map (Stage 1)

**What.** The `compact_osrs_v1` mode exists end to end: configuration in `modes/compact_osrs_v1/`,
a mode compiler (`tools/modec`), a rules loader that patches the definitions and logs every
override (`rc-core/mode.c`), the mode's world content (`rc-content/compact.c`), a validator
(`rs-gym-validate`) writing the override ledger, the reference map `maps/compact_v1.toml`, and a
headless smoke test (`tests/test_compact_mode.c`).

**Why.** Stage 1 of the v1 roadmap: register the mode, establish the manifest, ledger and validator
formats, build the map, and prove reset and step work headless while the island mode keeps running.

**How it fits.**
- Names live in TOML; `modec` resolves them against the cache, selects canonical `recipes.bin` rows
  by output and inputs, and refuses content that is not closed (an input nothing supplies, a station
  not placed, a reachable item without a sale value). Two unstrung bows have no creation row in the
  data snapshot and are authored from the OSRS Wiki, marked as such in the manifest and ledger.
- The engine reads one rules file per process (it is part of the game-data identity). Definition
  overrides happen at load, so every world shares the patched data; world rules (stations, shops,
  drops, death, loot timing, kit) install per world.
- Engine extensions, all inert without a mode: XP multiplier, enabled skills and level cap in
  `rc_add_xp_hundredths`; `level_offset` on gathering, fishing and cooking rules so a rebased unlock
  keeps the canonical success curve; hand-picked and never-depleting nodes; eight new production
  menus with per-loc options and batch altars; fixed-price shops whose sales remove items; NPC
  defence bonuses and elemental weakness fields on definitions; configurable NPC loot timing.
- The map is a 128×128 island (town with bank, general store, smithy, church, house with a range,
  spinning wheel and well; farm and river; woods and quarry; an outer resource area with two
  skeletons and two hill giants; essence and five altars) and a 64×64 dungeon made of the Edgeville
  hill giant cave and Scurrius' lair from the Varrock Sewers, joined by an obstacle pipe.

**Checked.** All nine ctest suites pass, including the island tests and the new compact smoke test
(rules active, kit, XP ×8 and the cap, fixed shop prices and sales, a chicken's closed drop owned by
its killer, a tree yielding logs). `rs-gym-validate` passes 676 checks and writes 217 overrides. The
viewer starts on the mode (`RUNEC_VIEWER_SMOKE=1`); it has not been played by hand.

## 2026-10-28: v1 becomes compact_osrs_v1

**What.** `SPEC.md` now defines v1 as the `compact_osrs_v1` mode from the design baseline 1.0
handoff: a questless compact progression sandbox with 15 skills capped at 40, closed production
chains, a fixed-price economy, player trading, mild death recovery, Scurrius as the shared boss, and
a versioned RL interface with benchmark wrappers. `TODO.md` lists the work by the handoff's stages.

**Why.** This is the agreed v1 direction. It replaces the earlier v1 draft as the benchmark.

**Decisions.**
- The first v1 draft (island map, KBD and Chaos Elemental, Firemaking, wilderness) is kept as the
  `island` mode instead of being deleted, since the handoff requires existing modes to keep working.
- Trading moves from v2 into v1. PvP stays out.

## 2026-10-25: v1 implementation

**What.** All six v1 milestones: a dense island plus dungeon, up to 16 players per world, six
skills, shops, death rules, two bosses, and an RL interface with a PufferLib 5.0 binding, replays
and a benchmark. Not yet tested by hand in the viewer or trained.

**Why.** v1 is meant to be a complete small game that single- and multi-agent RL methods can be
compared on, fast enough for hour-long experiments.

**World (`maps/gym_v1.toml`, `tools/gymmap`).**
- Spec v1: named areas (island and void), rivers, paint rects, stamps in any area, traversal
  links, zones (wilderness bands, multi-combat), fixed and shoreline NPC placement, static ground
  items, gym drop tables, spawned-loc templates and named landmarks.
- New compiler modules: `spatial.py` (traversal and area-flag tables), `nodes.py` (gathering
  nodes, now with stump and empty-rock replacements), `drops.py` (merges gym drop tables into the
  source table).
- Real stair and ladder rows move with each stamp; spec links add the dungeon entrances.
- `build_gym.py` writes `data/regions/gym.cfg` (start, respawn, the loaded mapsquare rect, landmarks)
  and the spawned-loc templates (`tools/cache_pipeline/export_spawn_objects.py`, copied from
  RuneC_v4 and changed to read the spec). `export_objects.py` gives depletable nodes their stump or
  empty-rock replacement mesh.

**Engine (`rc-core`).**
- Players: a `players[16]` array with `world->player` as the acting player; per-player ticks,
  interactions, command queues, banks, NPC targeting, hit sources and loot credit. The whole gym
  stays loaded (`pin_area`). `layout.c` reads `gym.cfg`; `rc_gym_world_config` is the one gym config
  for the viewer, tests and rc-env.
- `skilling.c`: per-player skilling jobs (gather, fish, firemaking, cook, make) with OSRS success
  curves; furnace/anvil products come from recipes.bin.
- `shops.c`: live shops with shared stock, stock-based prices and restocking.
- `death.c`: keep-3 (Protect Item), gravestones, wilderness drops, timed respawn.
- New player commands for making, lighting fires and shop buy/sell, so viewer and agent input queue
  for the next tick like every other command.
- `rc_world_reset_seeded` resets as a new world (RNG and NPC ids start over) for exact replays.
- Fixes found on the way:
  - Deaths are checked before prayer drain and regen, so Protect Item counts and regen cannot
    revive a killed player.
  - A handler's own result no longer replaces the chat message its game code published.
- Speed and threads:
  - Route searches use a 128×128 window (OSRS-sized) instead of 512×512.
  - Collision lookups remember the last map region.
  - The shared object-placement cache can be frozen for safe reads from many threads.
  - The combat-rejection log line only prints with `RC_DEBUG_COMBAT`.

**Content (`rc-content`).** `skilling.c` (tools, rates, fishing, logs, cooking, smithing products
and their handlers); `gym.c` (three shops, bone burying, death rules).

**RL (`rc-env`, new).** The C API, observation and action layouts, masks, ten task presets,
per-landmark travel fields, replays, `rs-gym-bench`, `rs-gym-rollout`, and the PufferLib binding with
`tools/puffer_overlay.sh`.

**Viewer.**
- The default player is a fresh gym player with the starter kit (dev 99s and bank:
  `RUNEC_DEV_VALIDATION=1`).
- A furnace/anvil make menu in the chatbox and a shop window in the bank panel.
- Skilling animations and Bury in the item menu.
- `--replay FILE`, with keys 1–9 to follow an agent; other players are drawn.

**Decisions.**
- Agents act through a flat 453-action masked space with entity lists, not raw tile clicks: walk
  hops and landmark travel keep every route short, which keeps stepping fast and learnable.
- Every route the engine searches stays OSRS-faithful (same BFS, smaller window).
- Shops, death and skilling rules are content tables; the engine holds the mechanics.
- I did not build against or link anything into an external PufferLib checkout; the overlay script
  does that when you run it.

**Verified.**
- `ctest`: 8 suites. Compiler tests (15), world, multiplayer, skilling (chop, mine, fish, light,
  cook, smelt, smith), rules (shops, death, burying), bosses (dragonfire with and without the
  shield, KBD drop, Chaos Elemental teleport and disarm), env (masked random play, determinism
  across envs and resets, scripted woodcutting reward, multi-agent episodes, boss death,
  record-and-replay), PufferLib binding (stand-in header).
- ThreadSanitizer is clean across all tasks with 4 threads.
- Viewer smoke run and a rendered replay frame.

**Known gaps.** See `TODO.md`.

## 2026-10-24: v1 spec

**What.** v0 is done and kept on branch `v0` (`ea600a0`). `SPEC.md` now defines v1 and v2, and
`TODO.md` is organised by v1 milestone.

**Why.** This is the agreed scope.
- v1 is a complete small game for single- and multi-agent RL, meant to become the main environment
  for new RL experiments.
- Trading, PvP, more monsters and encounters, and additional skills (still to be chosen) are v2.
- Quests, farming and minigames are not planned.

**Decisions.**
- The RL interface and several players per world are part of v1, so v1 can be used for
  experiments on its own.
- The multi-player engine change comes before the new systems, so skilling, shops and death are
  written for many players from the start.

## 2026-10-24: v0 island gym

**What.** New repo. A map compiler builds a 192×192 island out of b237 cache content; the copied
RuneC exporters turn it into runtime data; the copied engine simulates it and the copied viewer
shows it.

**Why.** Full OSRS is too large for agents to explore. A small, dense map keeps the interesting
systems close together. RS-gym is self-contained: it copies what it needs and never calls RuneC.

**Copied from RuneC_v4 at `338f2e6`:**
- `rc-core/`, `rc-content/`, `rc-viewer/`, `lib/raylib/`.
- The export closure: `tools/cache_pipeline/` (`rc_cache`, `export_scene_slice`, `export_terrain`,
  `export_objects`, `export_minimap`, `export_models`, `export_textures`, `export_collision_map`,
  `export_collision_map_modern`, `modern_cache_reader`, `source_inputs`), plus `tools/`
  `export_collision_tiles`, `export_object_placements`, `collision_tile_index`,
  `object_placement_index`, `region_sets`, `source_paths`, `spawn_index`.
- Not copied: RuneC's tests, guard scripts, docs and authored `content/` tree.

**Changed in the copies:**
- `rc-content/combat/magic.c`: includes `npc_spell_targets.inc` from `rc-content/combat/`, where it was
  moved from RuneC's `content/combat/`. It was the only include outside the three C components.
- `rc-viewer/viewer.c`, `dev_validation.{c,h}`:
  - Defaults now start at the gym (world origin 64,64; start 222,222 via `RS_GYM_START_X/Y`).
  - A failed start relocation exits with an error instead of leaving the player in the void.
  - The dev panel's real-world boss teleports and encounter tables are replaced by one Start
    teleport.
  - The Varrock bank combat dummy now spawns beside the gym start.
- `tools/export_collision_tiles.py`, `tools/export_object_placements.py`: reports go to
  `build/reports/` instead of the source tree.
- Removed `rc_cache/smoke.py`, `rc-core/AGENT.md` and the nested `rc-content` READMEs.
- `CMakeLists.txt` was rewritten to build only the three components and the two gym tests.

**New:**
- `tools/gymmap/`:
  - `mapformat.py` encodes and decodes b237 terrain and loc files.
  - `cachewrite.py` writes an overlay cache: the map index is replaced and every other index links
    to the source cache.
  - `compile.py` handles the spec: island terrain, paths, rotated stamps of real-world rects with
    height fitting, loc and scatter placement, NPC spawn placement.
- `tools/build_gym.py` runs compile → overlay cache → exporters → spawn files → empty area flags →
  `manifest.json`.
- `maps/gym_v0.toml`, `tests/test_gymmap.py`, `tests/test_gym_world.c`, and the docs.

**Data (local, git-ignored):**
- Copied the b237 cache to `data/source/`.
- Copied defs, models, sprites, fonts, ui and anims from RuneC_v4's installed
  `data-v2026.10.22-b237` release. Left out the legacy `collision_tiles.bin`, the Varrock NPC model
  bundles, `npcs.anims`, `player.anims` and `encounter_effects.atlas`, which nothing in RS-gym reads.
- Gym spatial data is generated.

**Verified:**
- All 2,869 real mapsquares round-trip through the encoder (4.88M locs).
- 10 Python tests and the C gym-world test pass under `ctest`.
- `RUNEC_VIEWER_SMOKE=1` passes with 26 NPCs.
- A rendered frame shows the island, buildings, NPCs and UI.

**Known gaps:**
- Skilling, shops, death rules, stairs and ladders, area flags and multi-player are not in v0 (see
  SPEC and TODO).
- `data/` cannot yet be rebuilt from inside RS-gym.
