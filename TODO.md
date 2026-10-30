# TODO

v1 is `compact_osrs_v1` (see `SPEC.md`). Stages 1-6 are done; the acceptance evidence is
`modes/compact_osrs_v1/acceptance-report.md` (`rs-gym-acceptance all --seeds 5`). Open work:

## Balance (found by the Stage 6 runs)
- Arrow production pays poorly: a fresh character makes 120 bronze arrows from the kit (about
  1,100 ticks) and no more, while the shop sells them for 1 coin; the ranged routes buy theirs.
  Consider more arrows per action or dearer shop arrows.
- Magic loses money on hill giants (runes and food cost about what they drop), so the magic route
  earns on goblins. Rune production (about 100 mind runes per 1,000 ticks from level 1) is the
  mage's real supply.
- Protection prayers count when an NPC attacks (the engine's rule), so Scurrius' even-odds bolts
  and fur in its final phase and while feeding cannot be answered after the cue. The routes pray
  ahead and flick on its attack timer. Decide whether its projectiles should check on landing.

## Evidence still missing
- Human playtesting: none. All acceptance runs are scripted.
- Trained agents: only the binding smoke test and random rollouts; no training results.
- The post-boss spine weapons and their paid alternative are covered by `tests/test_compact_boss.c`
  and `tests/test_compact_economy.c` with test setup, not by a scripted route.

## Map and viewer
- Scurrius' lair keeps no ceiling (its blank-out sat higher than the sewers' rock), so the lair's
  outer walls show dark faces from above.
- Random-action throughput is about a fifth lower than on the bare map: random walk targets are
  more often unreachable on the decorated island, and each failed search covers its window.

## Other
- Make `data/` reproducible inside RS-gym. Non-spatial data was seeded by copying RuneC_v4's
  installed release; ship it as a versioned archive or port the definition exporters.
- Viewer: remove RuneC-only paths (the real-world smoke scenes and the Varrock fixed-scene defaults).
