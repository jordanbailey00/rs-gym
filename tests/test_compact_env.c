// The compact-v2 RL interface (run tools/build_mode.py first): sizes and finite observations under
// random masked actions; identical seeds and actions give identical state hashes; a snapshot
// restores to the same future; Wait keeps an action and Stop ends it; changes an agent cannot see
// leave its observation and mask alone; stale and masked actions fail safely; the horizon
// truncates while a task success terminates with +1; a replay reproduces the episode.
#include "../rc-core/api.h"
#include "../rc-core/items.h"
#include "../rc-core/mode.h"
#include "../rc-core/npc.h"
#include "../rc-core/players.h"
#include "../rc-core/skills.h"
#include "../rc-env/compact_env.h"
#include "gym_test_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { SKIP = 77 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static float obs[RC_CENV_OBS_SIZE], obs2[RC_CENV_OBS_SIZE];
static uint8_t mask[RC_CENV_NUM_ACTIONS], mask2[RC_CENV_NUM_ACTIONS];

static uint32_t rng = 12345;
static uint32_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// A random valid action, preferring movement and interactions over waiting.
static int random_action(RcCompactEnv *env, int agent) {
    rc_cenv_action_mask(env, agent, mask);
    int valid[RC_CENV_NUM_ACTIONS], n = 0;
    for (int a = 1; a < RC_CENV_NUM_ACTIONS; a++) if (mask[a]) valid[n++] = a;
    return n ? valid[next() % (uint32_t)n] : RC_CACT_WAIT;
}

static RcCompactEnv *make(int agents, int profile, int horizon, uint32_t seed, const char *record) {
    RcCenvConfig cfg = {.mode = "compact_osrs_v1", .num_agents = agents, .profile = profile,
                        .horizon = horizon, .seed = seed, .public_map = true, .record_path = record};
    return rc_cenv_create(&cfg);
}

static void check_random_play(void) {
    RcCompactEnv *env = make(2, RC_CENV_DEVELOPMENT, 400, 7, NULL);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    int bad = 0, steps = 0;
    for (int t = 0; t < 400; t++) {
        for (int i = 0; i < 2; i++) {
            rc_cenv_observe(env, i, obs);
            for (int k = 0; k < RC_CENV_OBS_SIZE; k++) bad += !isfinite(obs[k]);
            rc_cenv_set_action(env, i, random_action(env, i));
        }
        steps++;
        if (rc_cenv_step(env)) break;
    }
    CHECK(bad == 0, "%d non-finite observation values", bad);
    CHECK(steps == 400 && rc_cenv_truncated(env, 0) && !rc_cenv_terminated(env, 0),
          "development horizon: %d steps, truncated %d", steps, rc_cenv_truncated(env, 0));
    const RcCenvMetrics *m = rc_cenv_metrics(env, 0);
    CHECK(m->results[RC_CENV_MASKED] == 0, "random masked play produced %d masked actions",
          m->results[RC_CENV_MASKED]);
    printf("random play: %d ok, %d stale, %d rejected actions\n", m->results[RC_CENV_OK],
           m->results[RC_CENV_STALE], m->results[RC_CENV_REJECTED]);
    rc_cenv_destroy(env);
}

static void check_determinism_and_snapshot(void) {
    RcCompactEnv *a = make(2, RC_CENV_FREE_PLAY, 100000, 99, NULL);
    RcCompactEnv *b = make(2, RC_CENV_FREE_PLAY, 100000, 99, NULL);
    CHECK(a && b, "env creation");
    if (!a || !b) return;
    int trace[300][2];
    int same = 1;
    for (int t = 0; t < 300; t++) {
        for (int i = 0; i < 2; i++) {
            trace[t][i] = random_action(a, i);
            rc_cenv_set_action(a, i, trace[t][i]);
            rc_cenv_set_action(b, i, trace[t][i]);
        }
        rc_cenv_step(a);
        rc_cenv_step(b);
        same &= rc_cenv_state_hash(a) == rc_cenv_state_hash(b);
    }
    CHECK(same, "identical seeds and actions diverged");
    // Snapshot at 300, run 150 more, restore, rerun the same 150: identical hashes.
    RcCompactSnapshot *snap = rc_cenv_snapshot(a);
    CHECK(snap != NULL, "snapshot");
    uint64_t hashes[150];
    int later[150][2];
    for (int t = 0; t < 150; t++) {
        for (int i = 0; i < 2; i++) rc_cenv_set_action(a, i, later[t][i] = random_action(a, i));
        rc_cenv_step(a);
        hashes[t] = rc_cenv_state_hash(a);
    }
    CHECK(rc_cenv_restore(a, snap), "restore");
    int match = 1;
    for (int t = 0; t < 150; t++) {
        for (int i = 0; i < 2; i++) rc_cenv_set_action(a, i, later[t][i]);
        rc_cenv_step(a);
        match &= rc_cenv_state_hash(a) == hashes[t];
    }
    CHECK(match, "a restored snapshot did not reproduce the uninterrupted run");
    rc_cenv_snapshot_free(snap);
    rc_cenv_destroy(a);
    rc_cenv_destroy(b);
}

static int move_action(int dx, int dy) {
    return RC_CACT_MOVE + (dy + RC_CENV_RADIUS) * RC_CENV_SIDE + dx + RC_CENV_RADIUS;
}

static void check_wait_stop_hidden_stale(void) {
    RcCompactEnv *env = make(2, RC_CENV_FREE_PLAY, 100000, 3, NULL);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    RcWorld *w = rc_cenv_world(env);
    rc_cenv_action_mask(env, 0, mask);
    // Walk somewhere ten tiles away, then wait: still walking. Stop: it ends.
    int far = -1;
    for (int d = 10; d >= 4 && far < 0; d--)
        for (int k = 0; k < 8 && far < 0; k++) {
            static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
            int a = move_action(dirs[k][0] * d, dirs[k][1] * d);
            if (mask[a]) far = a;
        }
    CHECK(far >= 0, "no walkable tile in the window");
    rc_cenv_set_action(env, 0, far);
    rc_cenv_step(env);
    int x0 = w->players[0].x, y0 = w->players[0].y;
    rc_cenv_set_action(env, 0, RC_CACT_WAIT);
    rc_cenv_step(env);
    CHECK(w->players[0].x != x0 || w->players[0].y != y0, "wait cancelled the walk");
    rc_cenv_set_action(env, 0, RC_CACT_STOP);
    rc_cenv_step(env);
    int x1 = w->players[0].x, y1 = w->players[0].y;
    rc_cenv_step(env);
    rc_cenv_step(env);
    CHECK(w->players[0].x == x1 && w->players[0].y == y1, "stop did not stop the walk");
    // Hidden state: the other agent's inventory and bank, a shop's stock, and the RNG.
    rc_cenv_observe(env, 0, obs);
    rc_cenv_action_mask(env, 0, mask);
    rc_world_select_player(w, 1);
    (void)rc_player_inventory_add(w, 379, 1, 0);         // test setup: unseen changes
    w->players[1].bank[5] = (RcInvSlot){.item_id = 1333, .quantity = 1};
    if (w->shop_count) w->shops[0].slots[0].stock = 1;
    w->rng_state ^= 0x5555;
    rc_cenv_observe(env, 0, obs2);
    rc_cenv_action_mask(env, 0, mask2);
    CHECK(!memcmp(obs, obs2, sizeof(obs)) && !memcmp(mask, mask2, sizeof(mask)),
          "agent 0 saw a change it cannot see");
    // A masked action and an out-of-range one fail safely.
    rc_cenv_action_mask(env, 0, mask);
    int masked = -1;
    for (int a = 0; a < RC_CENV_NUM_ACTIONS && masked < 0; a++) if (!mask[a]) masked = a;
    rc_cenv_set_action(env, 0, masked);
    rc_cenv_set_action(env, 1, RC_CENV_NUM_ACTIONS + 5);
    rc_cenv_step(env);
    CHECK(rc_cenv_last_result(env, 0) == RC_CENV_MASKED && rc_cenv_last_result(env, 1) == RC_CENV_OUT_OF_RANGE,
          "results %d %d", rc_cenv_last_result(env, 0), rc_cenv_last_result(env, 1));
    rc_cenv_destroy(env);
}

static void check_replay(const char *path) {
    RcCompactEnv *env = make(1, RC_CENV_DEVELOPMENT, 120, 21, path);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    while (!rc_cenv_step((rc_cenv_set_action(env, 0, random_action(env, 0)), env))) {}
    uint64_t final = rc_cenv_state_hash(env);
    rc_cenv_destroy(env);  // closes the recording
    RcCenvReplayHeader h;
    FILE *f = rc_cenv_replay_open(path, &h);
    CHECK(f && h.ticks == 120 && h.num_agents == 1 && !strcmp(h.schema, RC_CENV_SCHEMA),
          "replay header: %u ticks", f ? h.ticks : 0);
    if (!f) return;
    RcCompactEnv *again = make(h.num_agents, h.profile, h.horizon, h.seed, NULL);
    int actions[RC_CENV_MAX_AGENTS];
    while (rc_cenv_replay_next(f, &h, actions)) {
        rc_cenv_set_action(again, 0, actions[0]);
        rc_cenv_step(again);
    }
    fclose(f);
    CHECK(rc_cenv_state_hash(again) == final, "the replay ended in a different state");
    rc_cenv_destroy(again);
}

// Solo boss task: a strong character (test setup) fights through the interface; the kill
// terminates the episode with +1, and dying along the way would not end it.
static void check_solo_success(void) {
    RcCompactEnv *env = make(1, RC_CENV_SOLO_BOSS, 6000, 5, NULL);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    RcWorld *w = rc_cenv_world(env);
    const RcModeBoss *b = &rc_mode_active()->boss;
    RcPlayer *p = &w->players[0];
    rc_world_select_player(w, 0);
    for (int s = 0; s < SKILL_COUNT; s++) {
        p->skills.base_level[s] = p->skills.boosted_level[s] = 40;
        p->skills.xp[s] = RC_XP_TABLE[39];
    }
    p->max_hp = p->current_hp = 400;
    int gear[] = {1329, 1159, 1121, 1071, 1197};
    for (int i = 0; i < 5; i++) {
        const RcItemDef *d = rc_item_def_get(gear[i]);
        p->equipment[d->equip_slot] = (RcInvSlot){.item_id = gear[i], .quantity = 1};
    }
    for (int i = 0; i < 20; i++) (void)rc_player_inventory_add(w, 379, 1, 0);
    rc_recalc_bonuses(p);
    CHECK(rc_world_relocate_player(w, b->rect[0] + 1, b->centre[1], 0), "into the arena");
    float total = 0;
    int t = 0, weakened = 0;
    for (; t < 6000; t++) {
        rc_cenv_action_mask(env, 0, mask);
        rc_cenv_observe(env, 0, obs);
        int action = RC_CACT_WAIT;
        if (p->current_hp < 200) {
            int food = rc_inv_find(p->inventory, 379);
            if (food >= 0 && mask[RC_CACT_ITEM + food]) action = RC_CACT_ITEM + food;
        }
        for (int k = 0; action == RC_CACT_WAIT && k < RC_CENV_NPCS; k++) {
            const float *e = obs + RC_COBS_NPCS + k * RC_CENV_NPC;
            if (e[0] && e[3 + 7] && mask[RC_CACT_ATTACK + k]) action = RC_CACT_ATTACK + k;  // Scurrius
        }
        rc_cenv_set_action(env, 0, action);
        (void)rc_cenv_step(env);
        total += rc_cenv_reward(env, 0);
        if (w->mode_boss.active && !weakened) {  // test setup: interface semantics, not balance
            for (int i = 0; i < w->npc_count; i++)
                if (w->npcs[i].active && w->npcs[i].uid == w->mode_boss.boss_uid) w->npcs[i].current_hp = 25;
            weakened = 1;
        }
        if (rc_cenv_terminated(env, 0) || rc_cenv_truncated(env, 0)) break;
    }
    CHECK(rc_cenv_terminated(env, 0) && !rc_cenv_truncated(env, 0) && total == 1.0f,
          "solo boss: terminated %d truncated %d return %.1f after %d ticks", rc_cenv_terminated(env, 0),
          rc_cenv_truncated(env, 0), total, t);
    CHECK(rc_cenv_metrics(env, 0)->boss_credits == 1, "no boss credit in the metrics");
    printf("solo boss task: success after %d ticks\n", t);
    rc_cenv_destroy(env);
}

int main(void) {
    if (!gym_mode_built("compact_osrs_v1")) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    char path[512];
    const char *tmp = getenv("RS_GYM_TEST_TMP");
    snprintf(path, sizeof(path), "%s/test_compact_env.rgc", tmp ? tmp : "build");
    check_random_play();
    check_determinism_and_snapshot();
    check_wait_stop_hidden_stale();
    check_replay(path);
    check_solo_success();
    if (failures) return 1;
    printf("compact env: PASS\n");
    return 0;
}
