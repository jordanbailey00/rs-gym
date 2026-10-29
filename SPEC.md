# RS-gym spec

## v0: island map (done, branch `v0`)

A small island built from OSRS cache content, simulated by the copied engine and walkable in the viewer.

### Map

- 192×192 playable tiles (3×3 mapsquares) at world tiles 128–319, in cache coordinates the real
  world never uses (it starts at mapsquare 15, 19).
- A one-mapsquare ocean ring (world tiles 64–383) keeps the view and the engine's active window
  inside generated data.
- Generated island: wobbling coastline (radius 74 tiles, about ±15%), sand beach, two grass shades,
  rolling heights from the client's height noise, shallow-to-deep sea. All sea tiles are blocked.
- Real buildings copied with all four planes:
  - Draynor bank (bank booths, cooking fire)
  - Lumbridge general store, turned 90°
  - Lumbridge smithy (furnace, anvil), turned 270°
  - Varrock south-east mine (copper, tin, iron)
- Stone roads, 45 trees, 10 oaks, 10 willows.
- 26 NPCs: chickens, cows, goblins, rats, dark wizards, men.
- Player start: world tile 222, 222 on the town road.

### Systems (RuneC behaviour, unchanged)

| System | v0 |
|---|---|
| Movement, pathfinding, run energy | yes |
| Melee, ranged, magic combat; attack styles; prayer | yes |
| Inventory, equipment, consumables, NPC loot drops | yes |
| Banking at the Draynor booths | yes |
| Skilling | no: rocks and trees deplete only; no XP or products |
| Shops | no buy/sell |
| Death | no item loss or respawn point |
| Stairs and ladders in copied buildings | no (need traversal rows) |
| Wilderness or multi-combat zones | no (empty area-flag table) |
| Trading, PvP | no: one player per world |

### Pipeline

`maps/*.toml` → `tools/gymmap` → overlay cache (map index replaced) → copied exporters →
`data/regions`, `data/spawns`. Output is deterministic for a given spec and seed.

### Viewer

Starts at the gym start. The dev panel (Clan Chat tab) has plane buttons and a Start teleport. The
bank is seeded with test gear and a combat dummy stands beside the start.

### Verified

- Every real b237 mapsquare round-trips through the compiler's encoder.
- `tests/test_gymmap.py`: format, rotation, compile and overlay-cache tests.
- `tests/test_gym_world.c`: the world loads, the player starts on the island, the sea stops
  movement, NPCs spawn, the Draynor booth opens the bank.
- Viewer smoke run and a rendered frame.

## v1: compact_osrs_v1 (in progress)

v1 is the mode `compact_osrs_v1`: a complete, questless OSRS-derived progression sandbox built from the
game's own systems, content and assets. Characters prepare for expeditions, gather and fight, come back
with resources, produce or buy upgrades, trade, and attempt harder activities, up to a repeatable
Scurrius encounter. It supports human play in rc-viewer, headless single-agent RL, and several agents
sharing one world.

Adopted on 2026-10-28 from the design baseline 1.0 handoff (26 October 2026). Numerical values marked
as defaults are proposed balance settings, not canonical OSRS. They live in mode configuration; changes
are recorded in the override ledger with the evidence behind them.

The first v1 draft (island map, King Black Dragon and Chaos Elemental, Firemaking, wilderness) stays
available as the `island` mode. It is not the v1 benchmark.

### Fixed scope and boundaries

#### Required first version

- One compact overworld and one connected dungeon, using existing OSRS terrain, objects, NPCs, items, and visual assets.
- One safe town with banking, production facilities, shops, a tanner, and a prayer altar.
- Fifteen skills: Attack, Strength, Defence, Hitpoints, Ranged, Magic, Prayer, Mining, Smithing, Woodcutting, Fletching, Fishing, Cooking, Crafting, and Runecraft.
- Three main progression bands, initially at levels 1, 15, and 30; skill cap 40.
- Melee, ranged, and magic routes that can each progress from the standard starting state to a successful boss encounter.
- Renewable food, ammunition, rune, material, and coin sources.
- Seven ordinary enemy families: chicken, cow, giant rat, goblin, skeleton, dark wizard, and hill giant. Select one appropriate existing variant of each. Boss minions are encounter content in addition to this roster.
- Scurrius as one repeatable, shared-world boss, with its recognizable mechanics and a small post-boss equipment improvement loop.
- Personal inventories, equipment, banks, shops, death and recovery, direct player trading, and shared resources.
- Configurations for one, two, four, and eight players. The supported range is one through eight.
- A human-playable viewer, deterministic headless simulation, a versioned RL interface, benchmark wrappers, and replayable validation runs.

#### Excluded from this version

There are no quests, quest points, quest flags, quest dialogue prerequisites, map-fragment tasks, or quest completion checks. Remove inherited quest gates from selected content through this mode's access rules. Do not recreate a campaign through mandatory boss order or an obligatory delivery checklist.

Do not add another boss, region, skill, currency, or item family before the required world passes its end-to-end checks. Herblore, Farming, Slayer, Firemaking, Construction, Sailing, Agility progression, Hunter, Thieving, PvP, the Grand Exchange, player housing, pets, clue scrolls, and achievements that unlock areas are outside this version. Existing run energy remains part of movement; it does not require an Agility progression system here.

Do not invent hunger, thirst, equipment durability, permanent character classes, or a town-building system. Potions are restricted to the prayer-potion supply described below. No networking service or public MMO deployment is required to prove shared-world simulation.

### Mode architecture and content manifest

Use a named mode such as `compact_osrs_v1`, adapting the registration mechanism to the repository. Keep canonical definitions available to other modes.

Separate these concepts, using the project's native data format:

1. Base OSRS content definitions and mechanics.
2. Mode whitelist and mode-specific overrides.
3. World layout and spawn definitions.
4. Economy and encounter parameters.
5. Observation, action, reward, and benchmark schemas.

All modified levels, XP rates, statistics, prices, drop tables, stocks, respawn times, recipes, access rules, and encounter scaling must live in named mode configuration or a clearly identified mode extension. Avoid scattered special cases throughout the generic engine.

Produce a machine-readable content manifest. Every selected item, NPC, object, recipe, and service needs its canonical identity, resolved project ID, mode role, dependencies, source, and overrides. An override record must identify the inherited value or rule, the replacement, and why it changed. Use namespaced mode identifiers when necessary; retain the canonical item/NPC identity and assets.

Run a content validator at startup or build time. It must reject unknown IDs, absent required assets, unreachable mandatory ingredients, requirements above the cap, invalid bow/ammunition combinations, missing stations, out-of-scope drops or shop goods, and selected content that still requires a quest.

### Starting state and progression

#### Fresh character

Start each new character at the town bank. All skills begin at level 1 except Hitpoints, which begins at 10. Use the corresponding canonical cumulative XP for those starting levels. Health, prayer points, and run energy start full. Banks and progression are personal.

Issue the following once per fresh character. Bank items are available through the normal bank interface, not automatically accessible everywhere.

| Location | Initial contents |
| --- | --- |
| Equipped | Bronze dagger |
| Inventory | Four cooked shrimps |
| Bank | 100 coins; bronze axe; bronze pickaxe; hammer; knife; small fishing net; fly fishing rod; lobster pot; needle; 20 thread; 30 feathers; shortbow; 50 bronze arrows; staff of air; 50 mind runes; 50 water runes |

Resolve exact existing item names/IDs from the project. The kit opens several early choices; it does not define a class. Every character receives the same kit. Death never issues it again.

#### Initial numerical defaults

| Parameter | Default |
| --- | --- |
| Maximum level | 40 for every enabled skill |
| Major unlock bands | 1, 15, 30 |
| XP curve | Existing OSRS cumulative curve, capped at level 40 |
| XP multiplier | 8 times ordinary awarded XP for enabled skills |
| Additional Scurrius XP multiplier | 1; do not stack a special boss-training multiplier on top |
| Inventory | 28 slots with ordinary stacking and equipment behavior |
| Simulation step | One authoritative game tick |
| Nominal presentation time | 0.6 seconds per tick; headless execution never sleeps |
| Fresh-start benchmark horizon | 12,000 ticks, equivalent to 120 nominal minutes |

Keep fractional XP accurately using the existing representation or a documented fixed-point representation. Do not round away small XP events. XP at the cap must not overflow or unlock content outside the mode.

#### Progression bands

| Capability | Level 1 | Level 15 | Level 30 |
| --- | --- | --- | --- |
| Metal equipment and arrows | Bronze | Steel | Mithril |
| Bow | Shortbow | Oak shortbow | Willow shortbow |
| Ranged body | Leather body | Hardleather body | Studded body |
| Ranged legs | Leather chaps | Leather chaps remain useful | Studded chaps |
| Combat spells | Wind Strike and Water Strike | Wind Bolt and Water Bolt | Wind Blast and Water Blast |
| Food production | Shrimps, beef and chicken | Trout | Lobster |
| Runecraft | Air, water and mind runes | Chaos runes | Death runes |

The table rebases creation and use requirements throughout each chain. Set Attack requirements for the selected melee weapons, Defence requirements for their armour, Ranged requirements for bows/armour, Magic requirements for spells, and relevant production requirements to the stated band. Preserve other canonical requirements only where they remain reachable and serve an identified purpose. Record every change.

Preserve combat formulas, weapon attack speeds, equipment bonuses, food healing, spell costs and normal spell behavior initially. Wind is the name of the selected OSRS air-element spells. [S10] For the reference profile, apply within-tier elemental scaling using only this mode's unlocked spells: both selected Strike spells have base maximum hit 4, both Bolt spells 10, and both Blast spells 14 before other modifiers. This explicitly rebases the unlocks and prevents out-of-mode Earth/Fire spells from influencing damage. Validate the resulting three styles and change numerical values only with recorded evidence.

Magic initially uses the staff of air, wizard hat, and wizard robe. Its progression comes mainly from spell tiers, rune supply, levels and prayer. No invented robe tiers or full mystic equipment branch is required.

#### Prayer selection

Enable these existing prayers with mode requirements:

- Level 1: Thick Skin, Burst of Strength, Clarity of Thought, Sharp Eye, Mystic Will.
- Level 15: Rock Skin, Superhuman Strength, Improved Reflexes, Hawk Eye, Mystic Lore, and all three protection prayers.
- Level 30: Steel Skin, Ultimate Strength, Incredible Reflexes, Eagle Eye, Mystic Might.

Retain their ordinary effects, exclusivity rules, prayer-point drain and timing. Protection prayers unlock at 15 to avoid turning the small world's Prayer training into a long mandatory grind. They remain a useful preparation choice rather than an admission check. Do not include other prayers or special prayer-unlock items.

### Closed production and supply chains

Every active recipe must have obtainable inputs, a usable tool/station, reachable requirements, correct input/output quantities, real elapsed production time, and a destination for its output. The following is the complete intended recipe scope, including intermediate and byproduct items.

#### Mining and Smithing

Place copper, tin, iron, coal, mithril, and pure-essence sources. Copper/tin/pure essence require Mining 1; iron/coal require 15; mithril requires 30. Use ordinary gathering mechanics and pickaxe behavior with these requirements.

Use these bar recipes:

- One copper ore plus one tin ore produces one bronze bar.
- One iron ore plus two coal produces one steel bar.
- One mithril ore plus four coal produces one mithril bar. These are inherited OSRS material ratios. [S3]

Smelting uses the furnace. Smithing uses an anvil and hammer. Enable these existing products for bronze, steel and mithril: scimitar, mace, full helm, platebody, platelegs, kiteshield, axe, and arrowheads. Enable the bronze dagger recipe so the starter weapon is also obtainable. Use canonical material quantities and production timing from the data snapshot, with requirements rebased to the metal's band. Preserve distinct weapon attack types and equipment penalties.

Steel studs consume one steel bar, preserving the ordinary material input. [S5] They require Smithing 15 in this mode; their armour products require Crafting 30. Do not accidentally require a Smithing level beyond the cap.

Pickaxes are shop-supplied equipment in this version. Ordinary pickaxes do not have standard anvil recipes. [S11] Axe and pickaxe acquisition rules are deliberately different. Rebase tool-use requirements to Mining/Woodcutting 1, 15 and 30 for bronze, steel and mithril tools; their corresponding wield requirements must also stay within the cap. Gathering with a tool carried in inventory must work without requiring it to be equipped.

#### Woodcutting and Fletching

Use normal, oak and willow trees at Woodcutting 1, 15 and 30. Include flax and a spinning wheel.

- Normal logs and a knife produce arrow shafts.
- Arrow shafts and feathers produce headless arrows.
- Headless arrows and bronze/steel/mithril arrowheads produce matching arrows.
- Normal/oak/willow logs and a knife produce their corresponding unstrung shortbows.
- Flax at a spinning wheel produces bow strings with Crafting 1.
- An unstrung bow plus a bow string produces the corresponding finished bow. These preserve the ordinary bow/ammunition component relationships. [S4]

Keep inherited batch quantities, XP and timing, then apply the mode XP multiplier and level overrides. Arrow assembly requirements follow the arrow's band. Headless arrows and arrow shafts are available at level 1. Chickens and shops supply feathers. The selected bows must support their intended arrows. Preserve two-handed bow equipment rules.

#### Crafting and leather

Use cowhide, soft leather, hard leather, needle, thread, steel studs, and an existing tanner service.

- One cowhide plus the tanning fee produces soft leather or hard leather.
- Soft leather produces a leather body, leather chaps, or leather cowl at Crafting 1.
- Hard leather produces a hardleather body at Crafting 15.
- A soft-leather body plus steel studs produces a studded body at Crafting 30.
- Soft-leather chaps plus steel studs produce studded chaps at Crafting 30.

Use canonical leather quantities and thread consumption from the selected data snapshot. Hardleather is a parallel torso option, not an ingredient in the studded-body recipe. [S5] Tanning defaults are one coin per soft leather and three per hard leather.

#### Fishing and Cooking

Use a small fishing net for raw shrimps, a fly fishing rod plus feathers for raw trout, and a lobster pot for raw lobsters. Requirements follow the three bands. Keep the corresponding existing fish and fishing-spot assets.

Cows supply raw beef and chickens supply raw chicken. The town range cooks these and the selected fish. Use ordinary per-item cooking time, food consumption, healing and attack delays. Burnt outputs remain valid byproducts; do not return the raw input or grant successful-cooking XP for a burn.

Where a canonical success/burn formula expects the original unlock level, rebase the relevant thresholds consistently. Test that each newly unlocked food is practically cookable, not technically selectable but almost always burnt.

#### Runecraft and Magic

Use pure essence as the mode's only essence item; it supports the selected rune types. [S9] Make its source directly accessible and renewable; remove any quest and membership access prerequisites for selected mode content. Place existing air, water, mind, chaos and death altar interactions within the condensed world. Directly accessible altar objects avoid importing talismans, quest doors or unrelated travel systems.

Each altar converts carried pure essence into its corresponding rune. The reference profile produces one rune per essence for all five types; this is an explicit simplification of level-dependent multiple-rune yields. Preserve normal production interactions and per-essence XP, then apply the mode XP multiplier. Inputs are physical non-stackable essence, not a free abstract resource.

Provide runes through Runecraft, Aubury's shop, and selected dark-wizard drops. The staff of air retains its unlimited-air-rune effect when equipped; all other required runes are consumed normally. [S8] The spell whitelist contains the six spells in the progression table. No Telegrab, alchemy, rune teleports, additional spellbook or law-rune chain is required.

#### Prayer and prayer potions

Ordinary bones and hill-giant big bones provide Prayer XP through the existing bury action. The town altar restores prayer points. Keep ordinary prayer consumption during combat.

Prayer potions are a recurring, relatively expensive shop supply and a possible small boss reward. Support their existing dose variants, effects and empty vial. A four-dose bottle must provide exactly four doses. Use canonical effects against the modified character levels.

Herblore is not enabled. Do not add herbs or secondaries with no usable recipe. Do not require potions for entry to the boss. A careful player can use the town altar, make shorter trips, or buy potions for a longer expedition.

### World layout and renewable resources

Target approximately a 128 by 128 tile overworld and a 64 by 64 tile dungeon. Align these bounds to the project's native chunk format. Adjust exact dimensions if necessary while retaining short but consequential travel. Use one designed reference map before introducing procedural layouts.

| Region | Required content and role |
| --- | --- |
| Town | Banker, general shopkeeper, Aubury, Ellis, range, furnace, anvil, spinning wheel, altar, water source; safe recovery and processing |
| Farm and river | Chickens, cows, flax, shrimp and trout fishing; safe basic supplies and early production |
| Woods and quarry | Normal/oak/willow trees, copper/tin/iron and some coal; early and intermediate production routes |
| Outer resource area | Mithril, richer coal/willow access, lobster fishing; longer bank trips and nearby danger |
| Connected dungeon | Giant rats, goblins, skeletons, dark wizards, hill giants; branches with distinct rewards; Scurrius arena deeper inside |
| Rune sites | Reachable pure-essence source and five existing altars; travel and inventory capacity contribute to rune cost |

Place two of the budgeted skeletons and two of the budgeted hill giants along outer-resource routes, with the remaining members of those families in the dungeon. These are placements within the counts below, not additional spawns. Ensure advanced nodes or their bank routes actually intersect meaningful danger; the word "dangerous" alone does not satisfy the layout requirement. Keep at least one deliberate maneuver or alternate path available so danger creates a decision rather than unavoidable damage.

Relocate existing services and objects as needed. This is condensed geography, not a claim that their canonical locations are adjacent. Use existing names, appearances and interactions. Service dialogue may select a normal service; it must not create a quest state.

Do not place every resource beside the bank. Initial walking-distance targets are 10-25 ticks from bank to town services, 25-60 to basic resources, and 70-140 to advanced resources or the boss. Measure actual path lengths, including dungeon transitions. The world should contain alternate useful paths without becoming a maze of long empty corridors.

Initial shared spawn budgets for the reference map:

| Content | Initial count |
| --- | --- |
| Chickens / cows | 8 / 6 |
| Ordinary giant rats / goblins | 6 / 6 |
| Skeletons / dark wizards / hill giants | 6 / 4 / 4 |
| Normal / oak / willow trees | 8 / 6 / 4 |
| Copper / tin / iron / coal / mithril rocks | 4 / 4 / 4 / 6 / 4 |
| Flax sources / essence sources | 8 / 2 |
| Shrimp / trout / lobster fishing locations | 3 / 2 / 2 |
| Scurrius | One shared encounter |

Start with resource respawn defaults of 20 ticks for rocks and flax, 40 ticks for depleted trees, and 50 ticks for ordinary enemies. Preserve the inherited distinction between resource types: an essence source need not deplete like an ore rock, and fishing spots need not behave like one-use rocks. Pin any fishing-spot movement schedule to seeded simulation time and approved locations. Do not silently add randomness to map topology.

Keep stocks, spawns and respawn settings fixed between the published one-, two-, four- and eight-player profiles initially. They represent different contention levels. If validation requires population-specific resource presets, name and version them explicitly. Essential resources must remain renewable at every population; there is no requirement that every player can gather simultaneously without competition.

Most map access is open from the start. Tools and skill requirements govern activities. Enemies, distance, supplies and execution govern practical access. Keep the starting bank, basic food sources and recovery tools safely reachable. Avoid player body-blocking as a new mechanic; preserve the project's appropriate OSRS player-stacking rules.

### Ordinary encounters and drop tables

Choose existing NPC variants and preserve their animations, footprints, attack types and characteristic behavior. The following are initial mode health and maximum-hit targets for scaling the selected variants. Preserve the combat formula; record the actual stats/bonuses used to achieve each profile.

| Enemy | Initial HP / maximum hit | Guaranteed output | Additional output and purpose |
| --- | --- | --- | --- |
| Chicken | 3 / 1 | Bones, raw chicken, 3-7 feathers | Basic food and the shared fishing/ammunition ingredient |
| Cow | 8 / 1 | Bones, raw beef, cowhide | Food and leather-production chain |
| Giant rat | 6 / 1 | Bones | Low-risk dungeon practice; omit unusable meat types from the mode table |
| Goblin | 10 / 2 | Bones, 8-16 coins | One 1-in-8 roll for a selected bronze equipment piece; reliable cash route |
| Skeleton | 20 / 3 | Bones, 10-20 coins | One 1-in-4 roll for 5-15 steel arrows; distinct defense/weapon matchup |
| Dark wizard | 18 / 4 | Bones, 6-12 coins | One rune bundle: 60% mind 8-16, 25% chaos 3-6, 15% death 1-3; one independent 1-in-20 wizard hat/robe roll |
| Hill giant | 35 / 5 | Big bones, 20-35 coins | One 1-in-5 material roll: 2 iron ore or 2 coal, equally likely |

These tables intentionally replace larger inherited drop tables. Keep the whitelist closed: no clue scrolls, quest objects, unsupported herbs, inaccessible keys, or out-of-scope equipment. Samples use seeded RNG. Always preserve actual item creation and ownership through the normal drop system.

Retain or tune defense differences so equipment choice matters. At least one ordinary encounter should favor magic, one favor ranged positioning, and one favor melee or a physical attack type. For the reference profile, give skeletons a 50% Air weakness and hill giants a 50% Water weakness through the existing elemental-weakness system. These are explicit mode values: apply the engine's normal weakness effects to accuracy/damage rather than inventing a second formula. They give the two selected elements distinct useful matchups despite their equal within-tier base hits. Dark wizards should retain a useful ranged matchup, and at least one physical defense profile should distinguish scimitar and mace. Do not add arbitrary style immunities or force every character to train all styles.

### Scurrius as the shared advanced encounter

Use the existing Scurrius definition and encounter implementation if present. Otherwise implement its recognizable OSRS behavior from the referenced material and assets: multiple attack styles, readable attack cues, movement hazards, rat minions, feeding/phase behavior, and meaningful prayer timing. Do not substitute a stationary melee-only health bar. [S1, S2]

The arena is part of the connected shared world. Players can fight together. No key, quest, previous-boss kill, or payment is required for entry. Leaving and returning uses normal movement and travel.

#### Initial mode tuning

Use these provisional parameters, preserving canonical attack cadence and phase structure unless validation justifies a documented change:

- Base maximum HP: 240.
- Melee, ranged and magic maximum hits: 8 each before protection effects.
- Movement-hazard maximum damage: 6.
- Rat-minion HP: 10; maximum hit: 2.
- Initial defensive targets: approximately 40 Defence and 40 Magic, with inherited style bonuses reviewed so all three player styles remain practical. Record exact derived values rather than relying on this sentence as an implementation formula.
- Boss-specific XP bonus: disabled; ordinary combat XP with the global multiplier remains.
- Player healing from arena food piles: disabled in this mode. Keep boss feeding behavior and its visible cues. Scale boss healing to the reduced HP budget and pin the resulting numeric value.
- Respawn after a completed kill: 100 ticks.
- Reset abandoned encounter after 50 ticks with no living players in the arena. Clear boss/minion/projectile/hazard state and encounter membership together.

#### Explicit group scaling

Use a monotonic encounter participant set. A living player joins when entering the arena during the active encounter; include all living players already inside when the encounter starts. Count each character once per encounter.

Maximum HP is `240 * (1 + 0.75 * (participants - 1))`, rounded consistently. When a new participant joins an active fight, increase current HP by the same absolute amount as maximum HP; previously dealt damage remains dealt. Leaving, dying, or re-entering does not reduce the scale or count the same character twice. Reset membership only on boss reset or after a completed death has been finalized.

Track phase transitions and one-time triggers explicitly. In this mode, reached phases do not regress when scaling or healing raises HP; joining does not replay a completed phase event or restart attack/cue timers. Add participants only to a living encounter. Finalize contributions, eligibility, loot and the completion event before clearing encounter state, and resolve each kill exactly once. A late entry must never revive a dead boss.

Keep per-hit damage unchanged with group size. Use the encounter's existing multi-target cues/hazards, and bound simultaneous minions to eight. Pin exact spawn counts and targeting in the encounter configuration after inspecting the existing implementation. Do not dynamically scale other monsters or resources without a named configuration change.

This scaling is an intentional mode modification. Validate one-, four- and eight-player fights. Cooperation should help, while a naive group of fresh characters repeatedly attacking with starter supplies should not make the entire preparation economy irrelevant. Skilled low-equipment strategies are allowed; do not introduce invisible admission requirements to prevent them.

#### Rewards and equipment improvement

Each kill creates one world loot bundle, not a duplicated full bundle per player:

- 150 coins.
- One equal-probability resource bundle: 3 mithril ore, 12 chaos runes, or 40 feathers.
- One independent 1-in-3 chance of a Scurrius spine.
- One independent 1-in-5 chance of a prayer potion(4).

The spine is optional for progression. First kills must be feasible with ordinary equipment. Its reward loop is required implementation scope, even though acquiring it is optional gameplay.

Use existing bone mace, bone shortbow and bone staff items/assets. Override their recipes for the condensed equipment roster:

| Output | Mode recipe | Creation / use requirement |
| --- | --- | --- |
| Bone mace | Scurrius spine plus mithril mace; hammer at an anvil | Smithing 30 / Attack 30 |
| Bone shortbow | Scurrius spine plus willow shortbow; knife | Fletching 30 / Ranged 30 |
| Bone staff | Scurrius spine plus staff of air plus 100 chaos runes; chisel | Crafting 30 / Magic 30 |

The ordinary OSRS recipes use higher-tier bases; these replacements are explicit modifications. [S1] Preserve the hammer/anvil, knife and chisel tool relationships. [S12] Use actual resolved item names from the snapshot, which may differ slightly from descriptive labels in this table.

Start the bone mace and bow with their mode precursor's ordinary combat bonuses and a +2 maximum-hit bonus against rat-type enemies. For the bone staff, use its existing powered-spell mechanism with a fixed base maximum hit of 14, independent of effective Magic level, before the same +2 rat bonus and ordinary damage modifiers. Magic level still affects accuracy and the use requirement. This fixed damage baseline is a mode override. Use a four-tick base attack interval and preserve the bow's rapid-style reduction to three ticks. Keep the bow restricted to the mode's arrow roster. Record any retained special minion-target timing from the inherited encounter explicitly; it must not reset ordinary boss attack cooldowns accidentally.

The 100 chaos runes in the staff recipe initialize exactly 100 charges; they are not consumed a second time. One further chaos rune provides one charge and each powered cast consumes one. Follow existing charging/uncharging rules where supported and account for every rune. Do not let transformation, uncharging or trading duplicate runes or spines.

Place Historian Aldo by the dungeon entrance as an optional paid production service. He uses the same consumed ingredients plus 100 coins and can bypass the creation-skill requirement; equipment-use requirements still apply. Aldo supplies the tools and station for this service. No creation XP is awarded for the paid service. This gives a combat-focused character a route without training every production skill.

Retain canonical trade restrictions on the spine and bone weapons; record them in the manifest. All ordinary materials and equipment use their supported trade rules. If a spine cannot be picked up by another player after becoming public, keep its ownership behavior consistent with the selected untradeable-item implementation.

### Shops, prices and economic invariants

Use a general shopkeeper, Aubury, Ellis and Historian Aldo with mode-specific stock or services. The general shop can carry the small hardware/equipment selection; additional shop NPCs are unnecessary. Banker access is free.

Prices below are fixed mode defaults. They are unrelated to live Grand Exchange prices. Every shop good begins at its listed stock cap on a fresh reset. Shared stock regenerates on authoritative ticks, capped at its listed maximum. Services do not create items without consuming their specified inputs.

| Shop goods | Player purchase price | Stock cap / restock |
| --- | --- | --- |
| Hammer, knife, chisel, fishing implements, needle | 1 each | 16 per item / 1 every 10 ticks |
| Thread, feathers | 1 each | 500 per item / 25 every 20 ticks |
| Bronze / steel / mithril axe or pickaxe | 10 / 50 / 150 | 8 per item / 1 every 100 ticks |
| Scimitar or mace, bronze / steel / mithril | 30 / 150 / 450 | 4 per item / 1 every 200 ticks |
| Full helm, bronze / steel / mithril | 20 / 100 / 300 | 4 per item / 1 every 200 ticks |
| Platebody, bronze / steel / mithril | 60 / 300 / 900 | 4 per item / 1 every 200 ticks |
| Platelegs or kiteshield, bronze / steel / mithril | 40 / 200 / 600 | 4 per item / 1 every 200 ticks |
| Shortbow / oak shortbow / willow shortbow | 30 / 150 / 450 | 4 per item / 1 every 200 ticks |
| Leather / hardleather / studded body | 50 / 200 / 600 | 4 per item / 1 every 200 ticks |
| Leather / studded chaps; leather cowl | 40 / 400; 20 | 4 per item / 1 every 200 ticks |
| Bronze / steel / mithril arrows | 1 / 2 / 4 per arrow | 500 / 300 / 200; restock 25 / 15 / 10 every 20 ticks |
| Air / water / mind runes | 1 each | 500 each / 25 every 20 ticks |
| Chaos / death runes | 4 / 8 each | 200 / 100; restock 10 / 5 every 20 ticks |
| Staff of air; wizard hat; wizard robe | 150; 50; 100 | 8 per item / 1 every 100 ticks |
| Cooked shrimps / trout / lobster | 3 / 8 / 20 | 50 / 30 / 20; restock 5 / 3 / 2 every 30 ticks |
| Prayer potion(4) | 240 | 8 / 1 every 160 ticks |

The shop buys supported finished equipment/food/ammunition/runes at 40% of its purchase price, rounded down. Zero-value items may be discarded without generating coins. Define a mode value for the bronze dagger; default purchase value 10, sale value 4. It need not occupy permanent shop stock.

Buy-only raw materials have initial unit sale values: copper 2, tin 2, iron 4, coal 3, mithril ore 10, bronze/steel/mithril bars 8/20/50, logs/oak logs/willow logs 2/5/10, flax 1, bow string 3, cowhide 3, soft leather 4, hard leather 6, pure essence 1. They are not automatically resold to players. Products omitted from the price table must receive explicit values in the manifest, consistent with their components and role; unsupported items are not silently accepted.

Player sales pay coins and remove the sold items from the economic simulation by default. They do not increase a stock cap or create an arbitrary resale inventory. This simplified shop rule is a mode override; it keeps restocking and prices reproducible. Direct player trading still supports item exchange.

For partial prayer potions, buyback value is proportional to remaining doses using the four-dose bottle's buyback value. Empty vials and burnt food have zero sale value. No paid conversion may increase refundable value merely by splitting or recombining doses.

Selling gathered or manufactured goods is legitimate income. Validate zero-work and reversible buy/sell/convert cycles for unintended profit, and separately measure the profitability of real production. A timed production route may earn coins, but must not accidentally supply every desirable capability more efficiently than the rest of the world.

Retain demand for food, ammunition, runes and prayer potions after the first boss kill. Durable equipment does not need invented decay. The intended horizon is a bounded benchmark or play session; this version does not promise a permanent economy that stays unsaturated for years.

### Death, recovery and expedition rules

Preserve skills, XP, bank contents and character identity on death. Death cancels routes, target selection, production, trades and invalid pending interactions. Respawn at the town bank after 10 ticks, with full health/prayer/run energy and no replacement starter items or money.

Use existing gravestone/recovery code where possible, with this explicit mode policy:

- All remaining carried and equipped items move atomically into a private recovery container at the death location. Already consumed supplies remain consumed.
- Recovery containers are not lootable by other players in v1 and have no fee.
- A recovery container lasts 1,500 ticks. On expiry, remaining items move into the character's bank through a defined safe transfer; they are never duplicated. Reserve capacity or use the existing overflow mechanism so a full bank cannot delete items.
- Multiple deaths create separate recovery records or an equivalently safe existing representation. Never overwrite an older grave's contents.
- Recovering items is a normal ranged/adjacent interaction consistent with the engine, not a remote inventory grant.

This intentionally mild recovery model preserves time, travel and consumable costs while avoiding permanent progression loss. Returning to town by dying can be a legitimate costly choice. Do not reward death, item recovery, or re-equipping as new progress. If recovery balance changes, version the death profile rather than silently changing it.

Starter tools, food and coins remain reacquirable through normal shops, gathering, animals and goblins. Validate recovery from a state with an inaccessible grave and zero spendable cash, including unarmed combat and low-value sales. A benchmark reset is the only operation that creates a fresh character and its starter kit.

### Shared-world and cooperative behavior

One world owns all players, NPCs, resources, stocks, drops, trades and encounter state. Do not implement multiplayer as independent private copies with a messaging layer.

Personal state includes skills, equipment, inventory, bank and private recovery containers. Shared state includes resource depletion, monster health and respawns, shop stock, ground items and the Scurrius encounter. PvP is disabled. Players can specialize and exchange ordinary items; no permanent classes or mandatory partner-only interactions exist.

#### Loot ownership

For ordinary monsters, the player who dealt the most actual non-overkill damage owns the drop for 100 ticks. Break exact ties through a documented seeded rule. After that, tradeable drops become public for another 300 ticks and then expire. Apply the supported untradeable-item rules separately.

For Scurrius, capture the completion snapshot after the tick's combat damage and deaths resolve. Qualify players who are alive in the arena in that snapshot and dealt at least 5% of total actual non-overkill player damage in the encounter. Allocate the single bundle through a seeded rotation among qualified participants across encounters. Persist the rotation state until a world reset and include it in snapshots. Use the same 100-tick private and 300-tick public periods for its tradeable loot.

If no player qualifies, for example because the final projectile produces a mutually lethal outcome, assign loot ownership to the largest damage contributor through the deterministic tie-break rule. This fallback grants ownership only, not qualifying-kill credit. The solo benchmark therefore does not count a mutually lethal completion as success. Cooperative success requires at least one qualified participant from the fixed team roster; all members of that team receive the shared success signal. Finalize untradeable ownership separately without duplicating the bundle.

The team benchmark rewards team success even when a producer contributed through supplies rather than damage. Individual boss-credit metrics use the participation rule above. Log both; do not conflate them.

#### Atomic direct trading

Trades require a selected nearby partner, versioned offers, visible item/coin quantities, and confirmation from both participants against the same revision. Any offer change invalidates both confirmations.

Commit both sides atomically after rechecking ownership, offered quantities, available destination space, proximity and player state. Reserve offered quantities or use an equivalent validated mechanism so they cannot also be eaten, equipped, dropped or sold. Cancel safely on death, explicit cancellation, excessive separation, or invalidated capacity. No partial transfers, negative quantities, duplicate items, or balance overflow are permitted.

No special free transfer command, remote bank access or team supply teleport is required. Team play uses the same movement, inventory and trade rules as solo play.

### Tick, action and observation contracts

#### Authoritative tick

One environment step advances one world tick. Collect all players' actions from the same pre-tick state, then run the simulation's phases and return post-tick observations. Document the actual phase order after inspecting the engine. Reuse its established ordering of prayer, consumables, routes, movement, target selection, attacks, projectiles, damage, death and respawns.

Movement and target selection must remain independently expressible where permitted by the OSRS simulation. Changing a target must not inadvertently cancel a valid route. Equipment changes must not erase attack cooldowns. A complete update of player A must not let A observe a newer tick than player B.

Use a deterministic shared conflict rule for the last shop item, a ground item, or an exhausted resource. Reuse the engine's rule if appropriate; otherwise use seeded or rotating priority, not a permanent low-ID advantage. Simultaneous action submission does not require changing canonical damage-event ordering. Document whether mutually lethal attacks can both resolve and test it.

Rendering and wall-clock timing must not influence RNG, movement, cooldowns or resource respawn.

#### Action coverage

Use the existing structured or factorized action API. It must express:

- Keep, cancel or replace a movement destination; use available walk/run control.
- Keep, clear or change an attack target.
- Select supported attack style, spell and prayers.
- Eat, drink, equip, unequip, bury bones, use, drop and pick up items.
- Interact with visible NPCs, stations, ladders, doors and resource objects.
- Start, continue, interrupt and stop supported gathering/production.
- Deposit and withdraw through the bank interface.
- Buy and sell through a reachable shop interface.
- Offer, revise, confirm or cancel a direct trade.
- Wait without cancelling an existing action.

Wait and stop are different actions. Make-X or make-all may enqueue ordinary per-item production, but must consume normal time and inputs, stop safely on interruption, and respect output space. No instant full-inventory production or automatic expedition-restocking macro belongs in the default action space.

If several action heads can issue incompatible intentions in one tick, define and test their resolution order. Conditional legality masks must match that contract. Invalid or stale commands safely fail, advance the ordinary tick, and report a diagnostic reason. They do not crash, grant items, teleport the actor or invoke a privileged fallback.

Derive actual action dimensions from the final content manifest and the repository's integration. Version the schema; do not invent dimensions before inspecting it.

#### Actor observations

Use structured observations for the initial benchmark. Preserve human-readable rendering as a separate view of the same state. The actor needs its own position, health, prayer, energy, levels/XP, equipment, inventory, relevant cooldowns, activity, target and interaction state.

Expose nearby terrain, objects, resources, NPCs, other players, drops, and visible combat cues. Use the engine's visibility rules. If none exist, define a local 25 by 25 tile observation window centered on the actor, respect collision/occlusion rules as explicitly documented, and make entity overflow behavior deterministic. Prioritize the currently visible attack target and immediate visible hazards when packing bounded entity arrays. Once a target leaves visibility, do not retain its live hidden state in a protected slot; only an explicitly marked last-observed memory may remain.

Recipe definitions, selected item requirements and shop catalogs may be public knowledge. Live shop stock and bank contents are observed while the corresponding interface is accessible; use an availability/known flag rather than presenting inaccessible state as a real zero balance. Trade participants observe both parties' offers, revision numbers and confirmation states. Other players' private inventories beyond their disclosed offers, banks, undiscovered drops, future damage rolls and remote stock changes are hidden.

Do not reveal hidden state through action masks. Static map information may be exposed only through a documented public-map option shared by human and RL play. Keep the reference profile locally observed; a full-state debugging profile must have a separate name.

A centralized critic may receive privileged state through a separate training interface. Never merge that state into the decentralized actor input. Put detailed debug and replay data in an explicitly privileged diagnostics channel, not in a field silently fed to the policy.

### Benchmark wrappers, rewards and instrumentation

The underlying sandbox continues after ordinary deaths and boss kills. Wrappers define stopping and scoring independently of game content.

| Profile | Stopping rule | Primary result |
| --- | --- | --- |
| Free play | Explicit user stop or configured safety horizon | Continuing playable world |
| Solo fresh-start boss | First qualifying Scurrius kill or 12,000 ticks | Success rate and ticks to first success |
| Shared-world development | Fixed 12,000-tick horizon | Per-player development and encounter metrics |
| Cooperative fresh-start boss | First team Scurrius kill or 12,000 ticks | Team success and preparation/coordination cost |

For the solo and cooperative sparse baselines, award +1 once on task success and zero otherwise. In the cooperative profile, all team members receive the shared success signal, including supply producers. A failed horizon is a truncation; task completion is a termination. Temporary player death is neither. Preserve these distinctions through the existing RL binding. Gymnasium and PettingZoo document the standard single-agent and parallel multi-agent contracts. [S6, S7]

The shared-world development profile logs metrics without inventing a default aggregate progress score. A future reward model can use that instrumentation, but total wealth or skill-sum is not automatically a sound reward.

Optional training shaping and curricula must be separately named, versioned, and disabled in fresh-start evaluation. Snapshot starts, partial equipment, and boss-practice states can help train; they must not contaminate fresh-start results. Do not award positive reward merely for bank/inventory transfers, trading, dropping and recovering the same item, selling and repurchasing it, or repeatedly crossing a location boundary.

Record authoritative events for item creation/consumption/destruction, gathering, manufacturing, transactions, XP, skill unlocks, deaths, recoveries and encounters. Distinguish actual new production from ownership transfer.

Log at least: first useful crafted item, first intermediate/top-band equipment use, supplies produced and consumed, skills trained, coins earned/spent by source, successful trades, deaths, time spent recovering, boss attempts and kills, damage by style, resource contention, invalid-action reasons, and preparation versus combat time. These are diagnostic events, not mandatory progression tasks.

Each run records engine revision, content/override revision, map revision, interface schema, reward profile, seed, population, starter state, horizon and resource settings. Report world ticks and aggregate agent decisions separately, so an eight-player run is not presented as having the same total interaction budget as a solo run. Keep the same content tables across train/evaluation seeds. First vary stochastic outcomes; procedural held-out maps can follow after topology and dependency validation exist.

A full reset must clear all episode-owned state: banks, skills, kits, stock depletion, respawns, drops, graves, pending trades, production queues, boss participant sets, loot rotation, event counters and RNG. Snapshots must capture that state and reproduce uninterrupted execution. Include the world tick, pending timed events/projectiles, encounter phase and contribution records, interaction/trade reservations, arbitration state, every simulation RNG stream, and wrapper elapsed-time/success state. Restoring must not reseed or reissue starter items. Determinism hashes cover canonical state fields rather than memory addresses or uninitialized structure padding.

### Implementation stages

#### Stage 1 — Reuse map and mode registration

Inspect the project, resolve content IDs/assets, register the mode, establish configuration/manifest formats, and implement a headless reset/step smoke test. Keep existing modes operational. Build the dependency validator early so it guides content integration.

#### Stage 2 — Playable economy

Implement the town, resources, personal bank, shops, selected recipes, XP/unlocks and ordinary monsters. A human or scripted controller must be able to gather, process, equip, fight, earn coins, buy supplies and return to town through real interactions. Validate all three combat-style supply chains before boss integration.

#### Stage 3 — Encounter and recovery

Integrate Scurrius, its mechanics, group-scaling state, loot and bone-weapon recipes. Complete death, respawn and recovery. Demonstrate a full solo run from standard start through a boss kill without debug state grants.

#### Stage 4 — Shared-world play

Enable multiple actors in the same simulation. Complete contention, loot ownership, atomic trade, team evaluation and maximum-population behavior. Demonstrate useful division of labor through ordinary inventory and trading.

#### Stage 5 — RL integration and reproducibility

Complete observations, masks, action coverage, wrappers, sparse reward, metrics, snapshots, replay and vectorized execution. Run the integration's native interface checks and a bounded training/rollout smoke test. A trained high-performing agent is not required to establish that the environment is complete.

#### Stage 6 — Balance and final acceptance

Run the complete fresh-start routes, economic analysis and adversarial checks below. Adjust provisional values where evidence identifies a blocked or degenerate route. Freeze the resulting reference configuration, record the changes, and provide exact commands and replay evidence.

### Acceptance evidence

Do not declare completion until the following checks pass or a genuine blocker is explicitly reported. Use meaningful integration and behavioral tests; do not substitute a large count of tests that merely echo configuration values.

#### Playable progression

1. Complete three solo runs from the ordinary start: predominantly melee, ranged and magic. No debug item grants, level setting, instant crafting, teleporting, direct inventory mutation, or reduced validation-only boss.
2. Demonstrate at least one meaningful manufactured upgrade and one purchased/looted upgrade. No route may require every enabled skill to be trained.
3. Every enabled recipe and charge/dose state resolves to obtainable inputs and reachable tools/stations. Every selected piece of equipment and spell can be used below the cap.
4. Intermediate equipment must support a plausible boss attempt with sufficient execution and supplies. Top-band equipment should help without being an invisible admission condition.
5. The post-boss spine recipes work, including their paid alternative and charge accounting, while first success does not depend on a spine.

Use scripted controllers or recorded human action traces that invoke the normal APIs. Scripts may know the designed reference map, but must not mutate state or use hidden RNG to make decisions. Run each complete combat-style route across at least five fixed seeds. Report success rate, preparation ticks, fight ticks, deaths and consumable cost. Investigate consistent failures rather than reporting one lucky success.

#### Economy and recovery

6. Verify item and coin conservation through banking, equipping, dropping, trading, shop transactions, death and recovery. Legitimate gathering, crafting outputs, monster drops, NPC purchases and explicit sinks are recorded separately.
7. Test zero-work buy/sell/conversion loops, dose splitting, charge/uncharge transitions, trade reversals and repeated item recovery for unintended money or reward creation.
8. Measure activity throughput for food production, arrow production, rune production, goblin income, dark-wizard hunting, hill giants and Scurrius. Report time and useful output per activity. Check that one activity does not replace the entire economy.
9. Recover from death with zero cash, an inaccessible grave and an empty personal inventory. Confirm no repeated starter grant, no lost older grave, no bank-overflow deletion and no permanent progression blockage.
10. Useful food, ammunition and rune production remain available after first boss success. Drops and shops do not invalidate every production route.

#### Multiplayer and timing

11. Complete a four-player cooperative run with different production investments, at least one real trade, and a shared encounter.
12. Run eight-player contention over resources, shop stock, ground loot, trades and boss entry. Assert no negative stock, duplicated goods, permanently stranded essential progression, low-ID monopoly, or invalid encounter scaling.
13. Test trade versus eat/equip/drop, trade versus death, changed offers after acceptance, insufficient inventory space, separation and simultaneous competing transfers.
14. Test movement plus attack, prayer timing, food delays, equipment cooldowns, ranged line of sight, obstacle routing and NPC footprints.
15. In the underlying sandbox and fixed-horizon shared-world profile, one player's death or success leaves other players and world state intact. The cooperative success wrapper deliberately ends its task on team success, without changing the underlying game's rules. Late boss entry, departure, death and re-entry do not create health-scaling exploits.
16. Compare prepared groups with a naive starter-only group. Document whether preparation remains useful at four and eight players. Allow skillful low-gear strategies and avoid adding a hidden access gate to force a preferred route.

#### RL and reproducibility

17. Identical seed, configuration and action trace produce identical authoritative state hashes. Rendering on/off and frame-rate changes do not change outcomes. Snapshot/resume matches uninterrupted execution.
18. Change unobserved remote stock, another player's private inventory or an unseen future random outcome while holding observable state fixed; actor observations and masks remain unchanged.
19. Spaces, masks and action indices match the versioned schema. Stale actions fail safely. Waiting preserves ongoing actions; stop cancels them.
20. Fresh-start evaluation has no curriculum grants or training-only rewards. Time limits and success map correctly to truncation/termination. Death remains ordinary gameplay.
21. Headless multi-world rollouts run without graphics initialization or wall-clock sleeps. Report measured ticks/second, world count, population, hardware, build configuration and memory use; do not invent a throughput claim.

#### Human play and presentation

22. The viewer displays real selected OSRS assets and lets a human perform the same gathering, production, banking, shopping, trading, combat and recovery interactions.
23. Health, prayer, energy, inventory, equipment, levels, requirements, prices, stock, trade offers and relevant combat cues are inspectable. Hidden or unusable interface controls do not silently expose out-of-scope content.
24. No mandatory item is represented only by a placeholder, no invisible wall blocks a required station, and no interaction depends on an unimplemented dialogue or quest flag.

Automated interaction runs can establish much of this evidence. If no human play session actually occurred, label that evidence accurately rather than claiming manual playtesting.

### Reference material

References identify inherited OSRS concepts and interface semantics. This spec's numerical defaults, placements, drop selections, access overrides, scaling and replacement recipes are mode design decisions.

- **S1 — Jagex, Scurrius release.** Solo/group support, three combat styles, existing rewards and original bone-weapon recipes. https://secure.runescape.com/m=news/scurrius-the-rat-king?oldschool=1
- **S2 — Jagex, Scurrius design.** Intended prayer, movement and multiple-enemy demands. Use the release and current project snapshot over superseded proposal details. https://secure.runescape.com/m=news/scurrius-the-rat-king---mid-level-pvm-boss?oldschool=1
- **S3 — OSRS Wiki, bars.** Ordinary metal-processing dependencies. https://oldschool.runescape.wiki/w/Bar
- **S4 — OSRS Wiki, Fletching.** Bow, string and ammunition production. https://oldschool.runescape.wiki/w/Fletching
- **S5 — OSRS Wiki, steel studs.** Steel studs and soft-leather armour inputs. https://oldschool.runescape.wiki/w/Steel_studs
- **S6 — Gymnasium Env API.** Reset/step and termination/truncation semantics. https://gymnasium.farama.org/api/env/
- **S7 — PettingZoo Parallel API.** Joint action submission and per-agent results. https://pettingzoo.farama.org/api/parallel/
- **S8 — OSRS Wiki, staff of air.** Unlimited-air-rune equipment behavior. https://oldschool.runescape.wiki/w/Air_staff
- **S9 — OSRS Wiki, pure essence.** Rune-production input. https://oldschool.runescape.wiki/w/Pure_essence
- **S10 — OSRS Wiki, combat spells.** OSRS spell names, rune costs and within-tier damage scaling. https://oldschool.runescape.wiki/w/Combat_spells
- **S11 — OSRS Wiki, Mining.** Pickaxe acquisition and tool behavior. https://oldschool.runescape.wiki/w/Mining
- **S12 — OSRS Wiki, bone weapons.** Creation tools and canonical item details. https://oldschool.runescape.wiki/w/Bone_mace ; https://oldschool.runescape.wiki/w/Bone_shortbow ; https://oldschool.runescape.wiki/w/Bone_staff

Resolve uncertain item names, IDs, recipe quantities, animations and detailed encounter events against the selected project snapshot and these sources. Keep the final implementation's manifest authoritative for this mode.

## v2 (planned)

- More monsters, NPCs, bosses and encounters, once v1 passes its end-to-end checks.
- Additional skills and regions. Which ones is still to be decided.
- PvP.

## Not planned

Quests, Farming and minigames.
