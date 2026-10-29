# rc-env

The RL interfaces over an RS-gym world. A step is one game tick: every agent's action is collected
against the same pre-tick state, the world ticks once, and each agent observes the result.
Stepping is deterministic for a seed and an action sequence.

- `compact_env.h`: the `compact_osrs_v1` mode's interface, schema **compact-v2** (the v1 benchmark).
- `env.h`: the island mode's v1 interface with task presets (below, from "C API").

Data paths resolve under `RUNEC_DATA_ROOT` (default `data`, so run from the repo root or set it).
Envs may step on different threads; game data loads once per process (one mode per process).

## compact-v2 (`compact_env.h`)

```c
RcCenvConfig cfg = {.num_agents = 1, .profile = RC_CENV_SOLO_BOSS, .seed = 1, .public_map = true};
RcCompactEnv *env = rc_cenv_create(&cfg);       // mode: NULL = compact_osrs_v1
float obs[RC_CENV_OBS_SIZE];
uint8_t mask[RC_CENV_NUM_ACTIONS];
for (;;) {
    for (int i = 0; i < cfg.num_agents; i++) {
        rc_cenv_observe(env, i, obs);
        rc_cenv_action_mask(env, i, mask);
        rc_cenv_set_action(env, i, pick(obs, mask));
    }
    int all_done = rc_cenv_step(env);          // reward, terminated, truncated per agent
    if (all_done) rc_cenv_reset(env, next_seed());
}
```

A reset is a full one: a fresh world from the seed and fresh characters with the starter kit at the
bank. 1–8 agents.

### Profiles (wrappers)

| Profile | Ends | Reward |
|---|---|---|
| `RC_CENV_FREE_PLAY` | only at the horizon (a safety limit) | none |
| `RC_CENV_SOLO_BOSS` | an agent's first Scurrius kill credit (terminated) or the horizon (truncated) | +1 once on success |
| `RC_CENV_DEVELOPMENT` | the horizon (truncated) | none; metrics only |
| `RC_CENV_COOP_BOSS` | the first kill credited to any team member: every member terminates | +1 to every member |

The horizon defaults to the mode's 12,000 ticks. Death ends nothing: the character respawns at the
bank after 10 ticks. Kill credit needs at least 5% of the fight's actual damage while alive inside
the arena when the kill resolves.

### Observation (4,598 floats, `RC_COBS_*` offsets)

| Section | Size | Contents |
|---|---|---|
| self | 96 | position in the area, area, hitpoints, prayer, run and special energy, death and respawn countdown, attack style, attack target (offset, HP), attack timer, skilling job and next attempt, moving, open menu, shop, bank, trade stage, the 18 enabled prayers, autocast spell, incoming hits by style (melee, ranged, magic, typeless: count and ticks to land), in the arena, free slots, carried coins, equipment bonuses, combat level, trade request pending |
| skills | 15×3 | the enabled skills: level and boosted level over the cap, progress to the next level |
| inventory | 28×20 | per slot: present, 14 categories (food, potion, melee/ranged/magic weapon, armour, ammunition, rune, tool, ore or bar, fletching part, material, bones, other), tier, quantity, sale value, charges, item vocabulary index |
| equipment | 11×20 | the same per equipment slot |
| tiles | 3×25×25 | the window around the agent (row-major from the south-west): blocked, falling-brick shadow (1 when it lands this tick), occupied (NPC 0.5, player 1) |
| NPCs | 12×28 | the attack target first, then nearest: offset, family (seven enemies, Scurrius, its rats, service, fishing spot), HP, size, my target, targeting me, fighting, its attack style cue, Scurrius' phase and feeding, attackable, has another option, level |
| locs | 12×30 | nearest locs with a working option, not depleted: offset, kind (24: trees, rocks, essence, flax, furnace, anvil, range, wheel, five altars, bank, prayer altar, links, food pile, other), which of three options work |
| ground items | 8×24 | visible items, nearest first: offset, item features, mine, public |
| players | 7×10 | other players in the window: offset, HP, combat level, fighting, my trade partner, asked me to trade, dead, in the arena |
| hazards | 8×4 | falling-brick shadows, soonest first: offset, ticks to land |
| landmarks | 32×4 | with `public_map`: named places reachable on foot from here: bearing and walking distance |
| shop | 4+64×5 | while a shop is open: which, coins; per slot item, price, stock, affordable |
| bank | 2+48×4 | while the bank is open: fill; the first 48 occupied slots: item, quantity, sale value |
| menu | 12+16×4 | while a production menu is open: which; ready options: output, quantity, coin fee |
| trade | 10+2×8×4 | stage, partner offset, revision parity, both acceptances, offer sizes; both offers |
| episode | 16 | profile, time elapsed, task success, own kill credits, agents, horizon, terminated, truncated |

Hidden from the actor: other players' inventories and banks (only trade offers), banks and shop
stock while closed, items dropped privately for others, NPCs and items outside the window, the RNG
and damage rolls. Overflowing lists are cut in their fixed order. There is no occlusion inside the
window, as in OSRS. Static map knowledge is limited to the landmark bearings behind `public_map`,
the same named places the viewer's map shows.

### Actions (1,255, `RC_CACT_*` ranges)

wait (keeps whatever is under way) · stop (ends walking, fighting, making) · toggle run · walk to a
window tile (625) · travel toward a landmark (24-tile hops along the walking path) · attack listed
NPC *k* · NPC *k*'s first or second other option (trade, fish, talk) · loc *k*'s first three options
· inventory slot's first option (eat, drink, bury, wield) and last option (check, uncharge) · drop ·
unequip · one of the mode's item pairs (knife on logs, needle on leather, headless arrow on
arrowtips, chaos rune on the bone staff …) · pick up listed ground item *k* · toggle an enabled prayer
· attack style · autocast an enabled spell · make one / all of a shown menu option · deposit an item,
the inventory or the equipment · withdraw one / all of a shown bank slot · buy one / ten of a shop
slot · sell an item · close · ask listed player *k* to trade · offer an inventory item · take back an
offered item · accept the current revision · decline.

The mask uses only what the observation shows. A masked, out-of-range or stale action does nothing,
the tick still runs, and `rc_cenv_last_result` says why (`RcCenvResult`, a diagnostics channel the
observation does not carry).

### Metrics, events, determinism, replays

- `rc_cenv_metrics`: XP by skill, deaths, ticks dead, kills, boss kills, credits and attempts,
  trades, supplies produced and consumed, coins from sales, loot and trades, coins spent in shops
  and on services, damage dealt by style, first production and first band-15/30 equipment ticks,
  ticks in the arena, action results.
- `RcCenvConfig.event_log`: one line per authoritative event (XP, coins, production,
  consumption, kills, boss kills, trades, deaths), after a header with the schema, mode version,
  profile, agents, seed, horizon, start state and map option.
- `rc_cenv_state_hash`: canonical world and wrapper state (`rc-core/snapshot.c`), never addresses.
- `rc_cenv_snapshot` / `rc_cenv_restore`: everything the episode owns, including every RNG stream,
  pending hits, boss state, trades and the wrapper's time and success.
- `record_path`: the seed, profile and every action; `rc-viewer --replay FILE` re-simulates it and
  `rc_cenv_replay_open` / `rc_cenv_replay_next` read it.

### PufferLib 5.0 (`puffer/compact/`)

Env `rs_gym_compact` (`rs_gym_compact.h`, `binding.c`, `rs_gym_compact.ini`). PufferLib 5.0 has one
done buffer, so `terminals` is 1 at either end of an episode; the observation's episode section and
the logs (`terminated`, `truncated`) keep the two apart. Episodes autoreset in the step that ends
them. `tools/puffer_overlay.sh /path/to/PufferLib` links both envs into a checkout.

## Island v1 interface: C API (`env.h`)

```c
RcEnvConfig cfg = {.num_agents = 4, .task = RC_TASK_WOODCUTTING, .seed = 1};
RcEnv *env = rc_env_create(&cfg);         // game data loads once per process, shared by all envs
rc_env_reset(env, 1234);
float obs[RC_ENV_OBS_SIZE];
uint8_t mask[RC_ENV_NUM_ACTIONS];
for (;;) {
    for (int i = 0; i < 4; i++) {
        rc_env_observe(env, i, obs);
        rc_env_action_mask(env, i, mask);
        rc_env_set_action(env, i, pick(obs, mask));
    }
    int time_up = rc_env_step(env);        // rewards, terminals, episode stats per agent after this
    if (time_up) rc_env_reset(env, next_seed());
}
```

Data paths resolve under `RUNEC_DATA_ROOT` (default `data`, so run from the repo root or set it).
Envs may step on different threads.

## Observation (1,521 floats, `RC_OBS_*` offsets)

| Section | Size | Contents |
|---|---|---|
| self | 48 | position, dungeon/plane, wilderness level, multi, HP, prayer, run and special energy, status, combat, attack style, skilling job, open menu/shop/bank, free slots, coins, carried and banked value, combat and total level, moving, prayers |
| skills | 13×3 | attack, strength, defence, hitpoints, ranged, prayer, magic, woodcutting, mining, fishing, firemaking, cooking, smithing: base, boosted, progress to next level |
| bonuses | 14 | equipment bonuses |
| inventory | 28×20 | per slot: present, 16 item categories, quantity, value, tier |
| equipment | 11×3 | present, tier, value |
| NPCs | 8×16 | nearest first: offset, distance, HP, level, targeting me / targeted by me, attackable, boss, fishing spot, shopkeeper, max hit, aggressive, fighting someone else, size |
| objects | 8×16 | nearest usable trees, rocks, fires/ranges, furnace, anvil, bank booths, stairs/ladders, altar: offset, kind, level needed, usable now, option bits |
| ground items | 6×13 | offset, mine or public, value, quantity, coarse category |
| players | 4×8 | other agents nearby: offset, HP, level, in combat, dead |
| landmarks | 24×4 | bearing and walking distance to each named place, reachable |
| map | 11×11 | walkable tiles around the agent (north up) |
| shop | 32×3+4 | open shop: in stock, price, affordable per slot; which shop |
| bank | 16×8 | first 16 bank slots: value, quantity, coarse category |
| task | 16 | task one-hot, time elapsed and left, agent index, agent count, death ends |

## Actions (453, `RC_ACT_*` ranges)

wait · move 1/5/15 tiles in 8 directions (stops where blocked) · travel toward landmark *k* (12-tile
hops along the walking path) · attack NPC *k* · NPC *k* option 0 or 2 (fish, trade) · object *k*
option 0–2 · use inventory item (eat, drink, wield, bury) · drop · light logs · unequip · 8 prayers ·
attack style · toggle run · pick up ground item *k* · make furnace/anvil product (all) · buy 1 / buy 10
from shop slot · sell all of an item · deposit all of an item · deposit inventory · withdraw a bank
slot · close · autocast one of 8 strike/bolt spells. *k* indexes the lists in the last observation.

The mask marks actions the game would accept now (tools, levels, space, coins, open interfaces).

## Tasks

| Task | Kit and start | Reward | Length |
|---|---|---|---|
| total | starter kit, town | XP in all skills, +0.2 per level, −0.5 per death | 12,000 |
| combat | iron gear, town | combat XP, kills, −1 per death | 6,000 |
| woodcutting / mining / fishing | starter kit | XP in the skill | 3,000 |
| cooking | starter kit | cooking XP, some fishing/firemaking/woodcutting XP | 6,000 |
| smithing | starter kit | smithing XP, some mining XP | 6,000 |
| wealth | starter kit | net worth gained (coins plus item value) | 6,000 |
| kbd / chaos_elemental | level 70, rune gear, lobsters, at the boss | damage to the boss, +1 per kill, −1 per death (death ends the agent's episode) | 2,000 |

The starter kit is a bronze axe, pickaxe, small net, tinderbox, hammer, bronze sword and 25 coins.
Episode stats: score (the task metric), return, length, XP, levels, net worth, kills, deaths, boss kills.

## Replays

Set `record_path` to write each episode's seed and actions. `rc-viewer --replay FILE` re-simulates it
exactly; `rc_env_replay_open` / `rc_env_replay_next` read it.

## Tools

- `rs-gym-rollout [--task T] [--agents N] [--ticks T] [--seed S] [--policy random|scripted] [--record F]`
  (island v1); with `--mode compact_osrs_v1` it plays compact-v2 with random masked actions
  (development profile), prints each agent's metrics and the end-state hash, and `--record` writes
  a file `rc-viewer --replay` plays.
- `rs-gym-bench [--envs N] [--agents M] [--threads T] [--steps S] [--task T]`: agent steps per second,
  including observations and masks (island v1).
- `rs-gym-validate [mode]`: content checks, override ledger and walking distances.
- `rs-gym-acceptance [all|routes|coop|contention|economy|perf] [--seeds N] [--style S] [--report F]`:
  the compact mode's acceptance runs and throughput (see `modes/README.md`).

## Island PufferLib 5.0 (`puffer/`)

`rs_gym.h` (env header), `binding.c` (`puf_init/reset/step/render/close/log`) and `rs_gym.ini`. One
PufferLib env is one world with `num_agents` players; rewards are clamped to [−1, 1]; episodes
autoreset in the step that ends them; `perf` is the task score over a per-task target. Hook it into a
checkout with `tools/puffer_overlay.sh /path/to/PufferLib` and set `RUNEC_DATA_ROOT` to RS-gym's
`data/`. `puf_render` is a no-op: record episodes and replay them in rc-viewer.

## Scripted controllers

`script.h` runs agent plans as coroutines over one world: each plan is blocking C that acts only
through player commands (walk, interact, bank, shop, make, gather, fish, fight, pick up, bury) and
waits on the world clock. `tests/test_compact_economy.c` uses it to drive every production chain;
`tools/acceptance.c` builds whole fresh-start routes, a co-op team and contention runs on it.
