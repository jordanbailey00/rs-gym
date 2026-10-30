// Scurrius and the mode's recovery rules (run tools/build_mode.py first). Test setup gives the
// fighters level 40 and mode equipment directly; every fight action is a player command.
//   - a solo kill: the phases, a feeding trip, rat summons (at most twelve waves), falling bricks,
//     the named actions the viewer presents (attacks and their eating variants, the jump and its
//     debris, the screech, the roar in the centre), the body held for its five-tick death, one
//     loot bundle owned by the qualified killer, and the respawn 100 ticks later;
//   - group scaling: each participant adds 180 HP to the maximum and the current, once;
//   - an abandoned fight resets after 50 ticks with nobody inside;
//   - bone weapons: the rat bonus, the staff's charges (made with 100, one per cast, charged and
//     uncharged with chaos runes);
//   - death: carried and worn items wait privately, then move to the bank when they expire; with a
//     full bank they stay on the ground, still private, and bank as soon as a slot frees.
#include "../rc-core/api.h"
#include "../rc-core/combat.h"
#include "../rc-core/items.h"
#include "../rc-core/mode.h"
#include "../rc-core/npc.h"
#include "../rc-core/players.h"
#include "../rc-core/prayer.h"
#include "../rc-core/skills.h"
#include "../rc-core/storage.h"
#include "../rc-content/combat/magic.h"
#include "../rc-content/content.h"
#include "../rc-env/script.h"
#include "gym_test_config.h"

#include <stdlib.h>
#include <string.h>

enum {
    SKIP = 77, SCURRIUS = 7222, MINION = 7223, GIANT_RAT = 2856, LOBSTER = 379, COINS = 995,
    MITHRIL_SCIMITAR = 1329, MITHRIL_HELM = 1159, MITHRIL_BODY = 1121, MITHRIL_LEGS = 1071,
    MITHRIL_SHIELD = 1197, BONE_MACE = 28792, BONE_STAFF = 28796, CHAOS_RUNE = 562,
    MITHRIL_MACE = 1428,
};

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const RcModeBoss *B;

static RcNpc *boss(RcWorld *w) {
    for (int i = 0; i < w->npc_count; i++) {
        const RcNpcDef *d = w->npcs[i].active ? rc_npc_def_for_npc(w, &w->npcs[i]) : NULL;
        if (d && d->id == SCURRIUS) return &w->npcs[i];
    }
    return NULL;
}

// Test setup: level 40, mithril armour and scimitar, lobsters.
static void fighter(RcWorld *w, int slot, int lobsters) {
    rc_world_select_player(w, slot);
    RcPlayer *p = w->player;
    rc_content_compact_new_character(w);
    for (int s = 0; s < SKILL_COUNT; s++) {
        p->skills.base_level[s] = p->skills.boosted_level[s] = 40;
        p->skills.xp[s] = RC_XP_TABLE[39];
    }
    p->max_hp = p->current_hp = 400;
    p->current_prayer_points = 400;
    int gear[] = {MITHRIL_SCIMITAR, MITHRIL_HELM, MITHRIL_BODY, MITHRIL_LEGS, MITHRIL_SHIELD};
    for (int i = 0; i < 5; i++) {
        const RcItemDef *d = rc_item_def_get(gear[i]);
        p->equipment[d->equip_slot] = (RcInvSlot){.item_id = gear[i], .quantity = 1};
    }
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) p->inventory[i] = (RcInvSlot){.item_id = -1};
    for (int i = 0; i < lobsters; i++) (void)rc_player_inventory_add(w, LOBSTER, 1, 0);
    rc_recalc_bonuses(p);
}

static void enter_arena(RcWorld *w, int slot, int dx) {
    rc_world_select_player(w, slot);
    CHECK(rc_world_relocate_player(w, B->rect[0] + 1 + dx, B->centre[1], 0), "player %d into the arena", slot);
}

// Encounter-action keys the boss's attack events carry (rc-content/encounters/scurrius.c).
static const char *const ACTIONS[] = {
    "Tail swipe", "Flying fur", "Bolts of electricity", "(eating)", "Falling bricks:jump",
    "Falling bricks:debris", "Rats:screech", "Enrage",
};
enum { ACTION_COUNT = sizeof(ACTIONS) / sizeof(ACTIONS[0]) };

typedef struct {
    int saw_phase3, saw_feed, saw_rats, saw_bricks, max_hp_seen, max_summons;
    int actions[ACTION_COUNT];
} FightLog;

static void log_actions(const RcWorld *w, const RcNpc *s, FightLog *log) {
    int n = 0;
    const RcCombatAttackEvent *e = rc_combat_attack_events(w, &n);
    for (int i = 0; e && i < n; i++) {
        if (!e[i].active || e[i].source_uid != s->uid || e[i].action_kind != RC_COMBAT_ACTION_ENCOUNTER)
            continue;
        for (int k = 0; k < ACTION_COUNT; k++)
            if (strstr(e[i].action_key_name, ACTIONS[k])) log->actions[k]++;
    }
}

// Fights Scurrius with protection prayers matched to its cue, eating below half hitpoints.
static void fight_plan(ScAgent *a, void *arg) {
    FightLog *log = arg;
    RcWorld *w = sc_world(a);
    for (int t = 0; t < 3000; t++) {
        RcNpc *s = boss(w);
        if (!s || s->is_dead) return;
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) sc_fail(a, "died in the fight");
        if (w->mode_boss.phase >= 3) log->saw_phase3 = 1;
        if (w->mode_boss.feeding_pile >= 0) log->saw_feed = 1;
        if (w->mode_boss.minion_count) log->saw_rats = 1;
        if (w->mode_boss.hazard_count) log->saw_bricks = 1;
        if (w->mode_boss.max_hp > log->max_hp_seen) log->max_hp_seen = w->mode_boss.max_hp;
        if (w->mode_boss.summons > log->max_summons) log->max_summons = w->mode_boss.summons;
        log_actions(w, s, log);
        // Step off a brick's shadow.
        for (int h = 0; h < w->mode_boss.hazard_count; h++)
            if (w->mode_boss.hazards[h].x == p->x && w->mode_boss.hazards[h].y == p->y) {
                (void)rc_player_step(w, p->x > B->centre[0] ? -1 : 1, 0);
                break;
            }
        if (p->current_hp < 200) sc_eat(a);
        if (p->attack_target != s->uid) (void)rc_player_attack_npc(w, s->uid);
        sc_wait(a, 1);
    }
    sc_fail(a, "the fight did not end");
}

static void check_solo_kill(RcWorld *w) {
    fighter(w, 0, 20);
    enter_arena(w, 0, 0);
    RcNpc *s = boss(w);
    CHECK(s && !s->is_dead && s->current_hp == B->hp, "Scurrius not fresh at %d HP", s ? s->current_hp : -1);
    ScRun run;
    FightLog log = {0};
    sc_init(&run, w, 1, getenv("RS_GYM_SCRIPT_LOG") ? stderr : NULL);
    sc_start(&run, 0, fight_plan, &log);
    int ticks = sc_run(&run, w->tick + 3000);
    CHECK(run.agents[0].done && !run.agents[0].failed, "solo fight: %s after %d ticks",
          run.agents[0].failed ? run.agents[0].failure : "still running", ticks);
    sc_free(&run);
    CHECK(log.saw_feed && log.saw_rats && log.saw_bricks && log.saw_phase3,
          "fight mechanics: feed %d rats %d bricks %d phase3 %d", log.saw_feed, log.saw_rats,
          log.saw_bricks, log.saw_phase3);
    CHECK(log.max_hp_seen == B->hp, "solo max HP %d", log.max_hp_seen);
    CHECK(log.max_summons >= 1 && log.max_summons <= 12, "%d rat waves", log.max_summons);
    for (int k = 0; k < ACTION_COUNT; k++)
        CHECK(log.actions[k] > 0, "no '%s' action presented", ACTIONS[k]);
    s = boss(w);  // a tick after the death: the body stays for its five-tick death sequence
    CHECK(s && rc_npc_life_phase(s) == RC_NPC_LIFE_DYING && s->death_timer == 4,
          "the body has %d ticks left", s ? s->death_timer : -1);
    rc_world_tick(w);  // the kill resolves in the mode tick after the death
    CHECK(w->mode_boss.kills == 1 && w->mode_boss.last_owner == 0 && (w->mode_boss.last_qualified & 1),
          "kill: %u kills, owner %d, qualified %x", w->mode_boss.kills, w->mode_boss.last_owner,
          w->mode_boss.last_qualified);
    int coins = 0, bundle = 0;
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (!g->active) continue;
        if (g->item_id == COINS && g->quantity == 150 && g->owner_uid == 0) coins++;
        for (int k = 0; k < B->bundle_count; k++)
            if (g->item_id == B->bundle[k].item) bundle++;
    }
    CHECK(coins == 1 && bundle >= 1, "loot: %d coin piles of 150, %d bundle items", coins, bundle);
    rc_world_select_player(w, 0);  // out of the arena, so the respawned boss stays idle
    CHECK(rc_world_relocate_player(w, B->rect[0] - 10, B->centre[1] - 1, 0), "leave the arena");
    for (int t = 0; t < B->respawn_ticks + 20 && (!boss(w) || boss(w)->is_dead); t++) rc_world_tick(w);
    s = boss(w);
    CHECK(s && !s->is_dead && s->current_hp == B->hp, "Scurrius did not respawn fresh");
    printf("solo kill in %d ticks\n", ticks);
}

static void check_scaling_and_reset(RcWorld *w) {
    for (int i = 1; i <= 3; i++) fighter(w, i, 0);
    // Clear rats left from the solo kill, which persist like the real boss's.
    for (int i = 0; i < w->mode_boss.minion_count; i++) (void)rc_npc_remove(w, (RcNpcId)w->mode_boss.minions[i]);
    enter_arena(w, 1, 0);
    enter_arena(w, 2, 2);
    rc_world_tick(w);
    RcNpc *s = boss(w);
    int scaled = B->hp + B->hp * B->group_scale_pct / 100;
    CHECK(w->mode_boss.max_hp == scaled && s->current_hp == scaled, "two players: max %d current %d, want %d",
          w->mode_boss.max_hp, s->current_hp, scaled);
    rc_world_select_player(w, 1);
    (void)rc_player_attack_npc(w, s->uid);
    for (int t = 0; t < 12; t++) rc_world_tick(w);
    int dealt = scaled - s->current_hp;
    enter_arena(w, 3, 4);
    rc_world_tick(w);
    int three = scaled + B->hp * B->group_scale_pct / 100;
    CHECK(w->mode_boss.max_hp == three, "three players: max %d, want %d", w->mode_boss.max_hp, three);
    CHECK(s->current_hp >= three - dealt - 40, "late join kept damage dealt? current %d", s->current_hp);
    // Leaving and re-entering adds nothing.
    rc_world_select_player(w, 3);
    CHECK(rc_world_relocate_player(w, B->rect[0] - 12, B->centre[1] - 1, 0), "leave");
    rc_world_tick(w);
    enter_arena(w, 3, 4);
    rc_world_tick(w);
    CHECK(w->mode_boss.max_hp == three && __builtin_popcount(w->mode_boss.participants) == 3,
          "re-entry changed scaling: max %d participants %x", w->mode_boss.max_hp,
          w->mode_boss.participants);
    // Everyone leaves: 50 ticks later the fight resets.
    for (int i = 1; i <= 3; i++) {
        rc_world_select_player(w, i);
        (void)rc_world_relocate_player(w, B->rect[0] - 12 - i, B->centre[1] - 1, 0);
    }
    for (int t = 0; t < B->reset_ticks + 3; t++) rc_world_tick(w);
    s = boss(w);
    CHECK(!w->mode_boss.active && s->current_hp == B->hp && w->mode_boss.participants == 0
          && w->mode_boss.minion_count == 0, "abandoned fight: active %d hp %d participants %x rats %d",
          w->mode_boss.active, s->current_hp, w->mode_boss.participants, w->mode_boss.minion_count);
}

static const RcNpc *any_npc(const RcWorld *w, int def_id) {
    for (int i = 0; i < w->npc_count; i++) {
        const RcNpcDef *d = w->npcs[i].active ? rc_npc_def_for_npc(w, &w->npcs[i]) : NULL;
        if (d && d->id == def_id && !w->npcs[i].is_dead) return &w->npcs[i];
    }
    return NULL;
}

static void check_bone_weapons(RcWorld *w) {
    fighter(w, 4, 0);
    RcPlayer *p = w->player;
    const RcNpc *rat = any_npc(w, GIANT_RAT), *goblin = any_npc(w, 3029);
    p->equipment[EQUIP_WEAPON] = (RcInvSlot){.item_id = BONE_MACE, .quantity = 1};
    rc_recalc_bonuses(p);
    RcCombatCalc on_rat = rc_calc_melee(p, rat), on_goblin = rc_calc_melee(p, goblin);
    RcPendingHit hits[4];
    const char *why = NULL;
    w->combat_hooks.prepare_player_hits(w, rat, &on_rat, false, hits, 4, &why);
    w->combat_hooks.prepare_player_hits(w, goblin, &on_goblin, false, hits, 4, &why);
    CHECK(on_rat.max_hit == on_goblin.max_hit + 2, "bone mace: %d on a rat, %d on a goblin",
          on_rat.max_hit, on_goblin.max_hit);
    // The staff: made with 100 charges, one per powered cast, charged and uncharged with runes.
    p->equipment[EQUIP_WEAPON] = (RcInvSlot){.item_id = BONE_STAFF, .quantity = 1, .state_id = 100};
    rc_recalc_bonuses(p);
    RcCombatCalc calc;
    int speed = 5;
    CHECK(w->combat_hooks.prepare_player_magic(w, goblin, NULL, &calc, &speed, &why)
          && speed == 4 && calc.max_hit >= 14, "bone staff: max %d speed %d (%s)", calc.max_hit, speed,
          why ? why : "");
    CHECK(w->combat_hooks.consume_weapon_charge(w, BONE_STAFF)
          && p->equipment[EQUIP_WEAPON].state_id == 99, "a cast did not take one charge");
    p->equipment[EQUIP_WEAPON] = (RcInvSlot){.item_id = MITHRIL_MACE, .quantity = 1};
    (void)rc_player_inventory_add(w, BONE_STAFF, 1, 99);
    (void)rc_player_inventory_add(w, CHAOS_RUNE, 10, 0);
    int staff = rc_inv_find(p->inventory, BONE_STAFF), runes = rc_inv_find(p->inventory, CHAOS_RUNE);
    CHECK(rc_player_use_inventory_item_on_inventory_item(w, runes, staff), "use runes on the staff");
    rc_world_tick(w);
    staff = rc_inv_find(p->inventory, BONE_STAFF);
    CHECK(staff >= 0 && p->inventory[staff].state_id == 109 && rc_inv_find(p->inventory, CHAOS_RUNE) < 0,
          "charging: %u charges", staff >= 0 ? p->inventory[staff].state_id : 0);
    const RcItemDef *d = rc_item_def_get(BONE_STAFF);
    int uncharge = -1;
    for (int o = 0; o < RC_ITEM_ACTION_COUNT; o++) if (!strcmp(d->inventory_actions[o], "Uncharge")) uncharge = o;
    CHECK(uncharge >= 0 && rc_player_interact_inventory_item(w, staff, uncharge), "uncharge");
    rc_world_tick(w);
    int chaos = rc_inv_find(p->inventory, CHAOS_RUNE);
    staff = rc_inv_find(p->inventory, BONE_STAFF);
    CHECK(chaos >= 0 && p->inventory[chaos].quantity == 109 && staff >= 0 && !p->inventory[staff].state_id,
          "uncharging returned %d runes", chaos >= 0 ? p->inventory[chaos].quantity : 0);
}

static void check_death_recovery(RcWorld *w) {
    fighter(w, 5, 3);
    RcPlayer *p = w->player;
    int x = p->x, y = p->y;
    int bank_scimitars = 0;
    for (int i = 0; i < RC_BANK_SIZE; i++) bank_scimitars += p->bank[i].item_id == MITHRIL_SCIMITAR;
    // Test setup: the player takes lethal damage.
    p->current_hp = 0;
    rc_world_tick(w);
    CHECK(p->is_dead, "player did not die");
    int on_ground = 0, private_ = 1;
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (!g->active || g->x != x || g->y != y) continue;
        on_ground++;
        private_ &= g->owner_uid == 5 && g->bank_on_expiry;
    }
    CHECK(on_ground >= 6 && private_, "%d recovery items, private %d", on_ground, private_);
    for (int t = 0; t < 12 && p->is_dead; t++) rc_world_tick(w);
    CHECK(!p->is_dead && p->current_hp == p->max_hp, "no respawn after 10 ticks");
    CHECK(p->equipment[EQUIP_WEAPON].item_id < 0, "the kit came back on respawn");
    for (int t = 0; t < 1500; t++) rc_world_tick(w);
    int banked = 0, left = 0;
    for (int i = 0; i < RC_BANK_SIZE; i++) banked += p->bank[i].item_id == MITHRIL_SCIMITAR;
    for (int i = 0; i < w->ground_item_count; i++)
        left += w->ground_items[i].active && w->ground_items[i].owner_uid == 5;
    CHECK(banked == bank_scimitars + 1 && left == 0, "after expiry: %d scimitars banked, %d left on the ground",
          banked - bank_scimitars, left);
}

static void check_full_bank_expiry(RcWorld *w) {
    fighter(w, 4, 0);
    RcPlayer *p = w->player;
    // Test setup: a bank with every slot taken by other items.
    for (int i = 0; i < RC_BANK_SIZE; i++) p->bank[i] = (RcInvSlot){.item_id = 4000 + i, .quantity = 1};
    int x = p->x, y = p->y;
    p->current_hp = 0;
    rc_world_tick(w);
    CHECK(p->is_dead, "player 4 did not die");
    for (int t = 0; t < 1520; t++) rc_world_tick(w);
    int waiting = 0, private_ = 1;
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        waiting += g->active && g->bank_on_expiry && g->x == x && g->y == y && (g->owner_uid == 4 || g->original_owner_uid == 4);
        if (g->active && g->bank_on_expiry && g->x == x && g->y == y) private_ &= g->owner_uid == 4;
    }
    CHECK(waiting >= 5, "full bank: %d recovery items still waiting (expected the worn mithril)", waiting);
    CHECK(private_, "full bank: waiting recovery items became visible to other players");
    p->bank[0] = (RcInvSlot){.item_id = -1};  // a slot frees
    rc_world_tick(w);
    rc_world_tick(w);
    int banked = 0;
    for (int i = 0; i < RC_BANK_SIZE; i++) banked += p->bank[i].item_id >= 0 && p->bank[i].item_id < 4000;
    CHECK(banked == 1, "one freed slot banked %d recovery items", banked);
}

int main(void) {
    if (!gym_mode_built("compact_osrs_v1")) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config_mode("compact_osrs_v1", 6);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w || !rc_world_install_content(w, rc_content_register_all)) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    B = &rc_mode_active()->boss;
    for (int i = 0; i < 6; i++) {
        rc_world_select_player(w, i);
        CHECK(rc_world_relocate_player(w, GYM_START_X + i, GYM_START_Y, 0), "place player %d", i);
    }
    check_solo_kill(w);
    check_scaling_and_reset(w);
    check_bone_weapons(w);
    check_death_recovery(w);
    check_full_bank_expiry(w);
    rc_world_destroy(w);
    if (failures) return 1;
    printf("compact boss: PASS\n");
    return 0;
}
