# TODO

## v1 follow-ups
- Train a first baseline per benchmark task through PufferLib and record the scores in SPEC.
- Run `tools/puffer_overlay.sh` against a real PufferLib 5.0 checkout (the binding is only tested
  against a stand-in `pufferenv.h`).
- Report a hash of the gym data with `RC_ENV_VERSION`, so results stay comparable across rebuilds.
- Single-agent worlds run about 75k agent steps/s per thread; the per-tick NPC simulation is now
  the largest cost. Profile `rc_world_tick` (NPC movement and combat ticks).
- Observations describe items by category, value and tier; consider item-id embeddings with a
  custom PufferLib policy.
- Agents cannot open doors; either open every door at reset or add doors to the object list.
- Viewer: other agents are drawn in the local player's pose (no per-agent animation); the replay
  stops at the end of the recording instead of looping.
- Viewer: the shop window reuses the bank panel; give it its own layout and a Value-on-hover tooltip.
- Fishing spots never move (OSRS spots relocate every few minutes).
- Smoother coastline (diagonal overlay shapes); blend floors and heights where stamps meet
  generated ground.

## Other
- Make `data/` reproducible inside RS-gym. Non-spatial data was seeded by copying RuneC_v4's
  installed release; ship it as a versioned archive or port the definition exporters.
- Viewer: remove RuneC-only paths (the real-world smoke scenes and the Varrock fixed-scene defaults).
