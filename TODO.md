# TODO

v1 is `compact_osrs_v1` (see `SPEC.md`). Open work by stage:

## Stage 3: encounter and recovery
- Scurrius: attack styles and cues, falling rocks, rat minions, feeding, group scaling,
  participants, loot rotation, reset and respawn.
- Bone mace, bone shortbow and bone staff recipes, Historian Aldo's service, staff charges.
- Death: private recovery containers moving to the bank on expiry; respawn after 10 ticks.

## Stage 4: shared world
- Atomic player trading (engine, viewer, agent actions).
- Loot ownership by non-overkill damage (100 private, 300 public ticks), seeded tie-breaks.
- Rotating tick priority for contested resources, stock and ground items; up to 8 players.

## Stage 5: RL interface
- Versioned action and observation schema for the mode (local window, hidden-state rules,
  wait versus stop, trade actions), masks, diagnostics for failed actions.
- Benchmark wrappers (free play, solo boss, shared-world development, cooperative boss),
  sparse rewards, termination versus truncation, metrics and event logs.
- Snapshot and restore, canonical state hashes, replay, PufferLib binding.

## Stage 6: balance and acceptance
- Scripted fresh-start runs to a Scurrius kill for melee, ranged and magic over five seeds.
- Four-player cooperative run with a real trade; eight-player contention.
- Economy conservation and exploit tests, recovery tests, throughput, balance report.

## Other
- Make `data/` reproducible inside RS-gym. Non-spatial data was seeded by copying RuneC_v4's
  installed release; ship it as a versioned archive or port the definition exporters.
- Viewer: remove RuneC-only paths (the real-world smoke scenes and the Varrock fixed-scene defaults).
