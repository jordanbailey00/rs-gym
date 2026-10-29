# TODO

v1 is `compact_osrs_v1` (see `SPEC.md`). Open work by stage:

## Stage 5: RL interface (remaining)
- Viewer: replay compact-v2 recordings (`RGC2` files); today it replays v1 only.
- `tools/puffer_overlay.sh`: link `rs_gym_compact` too and check `data/modes/<mode>/gym.cfg`.
- `rc-env/README.md`: document the compact-v2 observation and action layouts field by field.
- Throughput: measure ticks/second for 1, 4 and 8 agents with observations and masks.

## Stage 6: balance and acceptance
- Balance: a level-40 mithril fighter without prayer loses to Scurrius (it deals about 0.8 HP a
  tick against 0.3); tune defence, max hits and feeding before the acceptance runs.
- Scripted fresh-start runs to a Scurrius kill for melee, ranged and magic over five seeds.
- Four-player cooperative run with a real trade; eight-player contention.
- Economy conservation and exploit tests, recovery tests, throughput, balance report.

## Other
- Make `data/` reproducible inside RS-gym. Non-spatial data was seeded by copying RuneC_v4's
  installed release; ship it as a versioned archive or port the definition exporters.
- Viewer: remove RuneC-only paths (the real-world smoke scenes and the Varrock fixed-scene defaults).
