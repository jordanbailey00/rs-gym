# rc-env

The RL interface over an RS-gym world. One env is one world holding 1–16 agents (players); a step is
one game tick in which every agent acts. Each agent gets a float observation and picks one action
from a flat discrete space with a validity mask. Task presets set rewards, starting kits and
episode length. Stepping is deterministic for a seed and an action sequence.

## C API (`env.h`)

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
- `rs-gym-bench [--envs N] [--agents M] [--threads T] [--steps S] [--task T]`: agent steps per second,
  including observations and masks.

## PufferLib 5.0 (`puffer/`)

`rs_gym.h` (env header), `binding.c` (`puf_init/reset/step/render/close/log`) and `rs_gym.ini`. One
PufferLib env is one world with `num_agents` players; rewards are clamped to [−1, 1]; episodes
autoreset in the step that ends them; `perf` is the task score over a per-task target. Hook it into a
checkout with `tools/puffer_overlay.sh /path/to/PufferLib` and set `RUNEC_DATA_ROOT` to RS-gym's
`data/`. `puf_render` is a no-op: record episodes and replay them in rc-viewer.

## Scripted controllers

`script.h` runs agent plans as coroutines over one world: each plan is blocking C that acts only
through player commands (walk, interact, bank, shop, make, gather, fish, fight, pick up, bury) and
waits on the world clock. `tests/test_compact_economy.c` uses it to drive every production chain.
