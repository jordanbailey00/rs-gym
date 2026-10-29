#include "skilling.h"

#include "api.h"
#include "config.h"
#include "interaction.h"
#include "items.h"
#include "npc.h"
#include "object_runtime.h"
#include "objects.h"
#include "pathfinding.h"
#include "player_command.h"
#include "rng.h"
#include "skills.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const RcSkillingRules *rules_of(const RcWorld *world) {
    return world && (world->enabled & RC_SUB_SKILLS) ? world->skilling : NULL;
}

static void say(RcPlayer *p, const char *message) {
    rc_interaction_publish_message(p, message);
}

static void sayf(RcPlayer *p, const char *fmt, const char *a, int n) {
    char buf[RC_INTERACTION_RESULT_MESSAGE_LEN];
    if (a) snprintf(buf, sizeof(buf), fmt, a);
    else snprintf(buf, sizeof(buf), fmt, n);
    say(p, buf);
}

// Item name in lower case for chat messages ("Raw shrimps" -> "raw shrimps").
static const char *item_noun(int item_id, char *buf, size_t cap) {
    const RcItemDef *def = rc_item_def_get(item_id);
    snprintf(buf, cap, "%.47s", def && def->name[0] ? def->name : "item");
    for (char *c = buf; *c; c++) *c = (char)tolower((unsigned char)*c);
    return buf;
}

int rc_skill_chance_256(int level, int low, int high, int scale_pct) {
    if (level < 1) level = 1;
    if (level > 99) level = 99;
    int64_t num = ((int64_t)low * (99 - level) + (int64_t)high * (level - 1))
                * scale_pct;
    int64_t den = 98LL * 10 * 100;
    int64_t chance = (num + den / 2) / den + 1;
    return chance > 256 ? 256 : (int)chance;
}

static int roll(RcWorld *world, int chance_256) {
    return (int)rc_rng_bounded(&world->rng_state, 256) < chance_256;
}

static int level_of(const RcPlayer *p, int skill) {
    return p->skills.boosted_level[skill];
}

static int has_item(const RcPlayer *p, int item_id) {
    return item_id > 0 && rc_inv_find(p->inventory, item_id) >= 0;
}

static int inventory_full(const RcPlayer *p) {
    return rc_inv_free_slot(p->inventory) < 0;
}

static void award_xp(RcPlayer *p, int skill, int xp_tenths) {
    if (xp_tenths > 0) rc_add_xp_hundredths(&p->skills, (RcSkill)skill,
                                            (int64_t)xp_tenths * 10);
}

// Replaces the item in `slot` with `item_id` (cooking turns raw into cooked).
static int replace_slot_item(RcWorld *world, int slot, int item_id) {
    RcItemTransaction tx;
    RcPlayer *p = world->player;
    RcItemActionResult r = rc_item_tx_begin(&tx, world);
    if (r.code == RC_ITEM_RESULT_OK)
        r = rc_item_tx_remove_slot(&tx, slot, 1, p->inventory[slot].generation);
    if (r.code == RC_ITEM_RESULT_OK) r = rc_item_tx_add(&tx, item_id, 1, 0);
    return r.code == RC_ITEM_RESULT_OK
        && rc_item_tx_commit(&tx).code == RC_ITEM_RESULT_OK;
}

void rc_skilling_install(RcWorld *world, const RcSkillingRules *rules) {
    if (!world) return;
    world->skilling = rules;
    world->make_option_count = 0;
}

int rc_skilling_add_make_option(RcWorld *world, int recipe,
                                RcMakeMenu facility, int success_pct) {
    if (!world || !rc_recipe_get(recipe)
            || world->make_option_count >= RC_MAKE_MAX_OPTIONS
            || success_pct <= 0 || success_pct > 100) {
        return -1;
    }
    world->make_options[world->make_option_count] = (RcMakeOption){
        .recipe = (int16_t)recipe, .facility = (uint8_t)facility,
        .success_pct = (uint8_t)success_pct,
    };
    return world->make_option_count++;
}

const RcGatherRule *rc_skilling_gather_rule(const RcWorld *world,
                                            int obj_id) {
    const RcSkillingRules *r = rules_of(world);
    for (int i = 0; r && i < r->gather_count; i++)
        if ((int)r->gather[i].obj_id == obj_id) return &r->gather[i];
    return NULL;
}

const RcFishRule *rc_skilling_fish_rule(const RcWorld *world, int npc_id,
                                        int option) {
    const RcSkillingRules *r = rules_of(world);
    for (int i = 0; r && i < r->fish_count; i++)
        if ((int)r->fish[i].npc_id == npc_id && r->fish[i].option == option)
            return &r->fish[i];
    return NULL;
}

const RcFireRule *rc_skilling_fire_rule(const RcWorld *world, int logs) {
    const RcSkillingRules *r = rules_of(world);
    for (int i = 0; r && i < r->fire_count; i++)
        if ((int)r->fires[i].logs == logs) return &r->fires[i];
    return NULL;
}

const RcCookRule *rc_skilling_cook_rule(const RcWorld *world, int raw_item) {
    const RcSkillingRules *r = rules_of(world);
    for (int i = 0; r && i < r->cook_count; i++)
        if ((int)r->cook[i].raw == raw_item) return &r->cook[i];
    return NULL;
}

int rc_skilling_is_cook_object(const RcWorld *world, int obj_id) {
    const RcSkillingRules *r = rules_of(world);
    for (int i = 0; r && i < r->cook_object_count; i++)
        if ((int)r->cook_objects[i] == obj_id) return 1;
    return 0;
}

const RcSkillTool *rc_skilling_best_tool(const RcWorld *world,
                                         const RcPlayer *p, int skill) {
    const RcSkillingRules *r = rules_of(world);
    if (!r || !p) return NULL;
    const RcSkillTool *tools = skill == SKILL_WOODCUTTING ? r->axes
                             : skill == SKILL_MINING ? r->pickaxes : NULL;
    int count = skill == SKILL_WOODCUTTING ? r->axe_count : r->pickaxe_count;
    int level = level_of(p, skill);
    for (int i = 0; tools && i < count; i++) {
        if (level < tools[i].level) continue;
        if (has_item(p, (int)tools[i].item_id)
                || p->equipment[EQUIP_WEAPON].item_id == (int)tools[i].item_id)
            return &tools[i];
    }
    return NULL;
}

static void begin_job(RcWorld *world, RcSkillJobKind kind, int rule,
                      RcTick first_attempt) {
    RcPlayer *p = world->player;
    memset(&p->skill_job, 0, sizeof(p->skill_job));
    p->skill_job.kind = (uint8_t)kind;
    p->skill_job.rule = (int16_t)rule;
    p->skill_job.started_tick = world->tick;
    p->skill_job.remaining = -1;
    p->skill_action = (int)kind;
    p->skill_ready_tick = world->tick + first_attempt;
    p->make_menu = RC_MAKE_MENU_NONE;
}

void rc_skilling_stop(RcPlayer *p) {
    if (!p) return;
    p->skill_action = 0;
    p->skill_ready_tick = 0;
    p->skill_job.kind = RC_SKILL_JOB_NONE;
}

void rc_skilling_close_make(RcPlayer *p) {
    if (p) p->make_menu = RC_MAKE_MENU_NONE;
}

// ---- Woodcutting and mining ----------------------------------------------

static const char *gather_tool_missing(int skill) {
    return skill == SKILL_WOODCUTTING
        ? "You need an axe to chop down this tree."
        : "You need a pickaxe to mine this rock.";
}

int rc_skilling_start_gather(RcWorld *world, int obj_id, int x, int y,
                             int plane) {
    const RcGatherRule *rule = rc_skilling_gather_rule(world, obj_id);
    const RcGatheringNode *node = rule
        ? rc_gathering_node_find(obj_id, x, y, plane) : NULL;
    if (!node) return 0;
    RcPlayer *p = world->player;
    const RcSkillTool *tool = rc_skilling_best_tool(world, p, rule->skill);
    char noun[48];
    if (!tool) {
        say(p, gather_tool_missing(rule->skill));
        return 0;
    }
    if (level_of(p, rule->skill) < rule->level) {
        sayf(p, rule->skill == SKILL_WOODCUTTING
                ? "You need a Woodcutting level of %d to chop down this tree."
                : "You need a Mining level of %d to mine this rock.",
             NULL, rule->level);
        return 0;
    }
    if (inventory_full(p)) {
        sayf(p, "Your inventory is too full to hold any more %s.",
             item_noun((int)rule->product, noun, sizeof(noun)), 0);
        return 0;
    }
    int ticks = rule->skill == SKILL_WOODCUTTING
              ? world->skilling->woodcut_ticks : tool->ticks;
    begin_job(world, RC_SKILL_JOB_GATHER, (int)(rule - world->skilling->gather),
              ticks);
    p->skill_job.target = obj_id;
    p->skill_job.placement_key = node->placement_key;
    p->skill_job.x = (int16_t)x;
    p->skill_job.y = (int16_t)y;
    p->skill_job.plane = (int8_t)plane;
    say(p, rule->skill == SKILL_WOODCUTTING ? "You swing your axe at the tree."
                                            : "You swing your pick at the rock.");
    return 1;
}

static void deplete_node(RcWorld *world, const RcGatheringNode *node) {
    RcObjectPlacement base;
    if (!rc_object_placement_find_key(node->placement_key, node->x, node->y,
                                      node->plane, &base))
        return;
    if (node->replacement_obj_id >= 0) {
        RcObjectPlacement replacement = base;
        replacement.obj_id = (uint32_t)node->replacement_obj_id;
        (void)rc_world_object_replace(world, &base, &replacement,
                                      node->respawn_ticks,
                                      RC_OBJECT_STATE_DEPLETED);
    } else {
        (void)rc_world_object_delete(world, &base, node->respawn_ticks,
                                     RC_OBJECT_STATE_DEPLETED);
    }
}

static void gather_attempt(RcWorld *world, RcPlayer *p) {
    const RcSkillingRules *r = world->skilling;
    RcSkillJob *job = &p->skill_job;
    const RcGatherRule *rule = &r->gather[job->rule];
    const RcGatheringNode *node = rc_gathering_node_find(
        job->target, job->x, job->y, job->plane);
    RcObjectPlacement current;
    if (!node || !rc_world_object_current_placement(
            world, job->target, job->x, job->y, job->plane,
            job->placement_key, &current)) {
        rc_skilling_stop(p);  // depleted, maybe by another player
        return;
    }
    const RcSkillTool *tool = rc_skilling_best_tool(world, p, rule->skill);
    if (!tool) {
        say(p, gather_tool_missing(rule->skill));
        rc_skilling_stop(p);
        return;
    }
    int woodcutting = rule->skill == SKILL_WOODCUTTING;
    p->skill_ready_tick = world->tick + (woodcutting ? r->woodcut_ticks
                                                     : tool->ticks);
    int chance = rc_skill_chance_256(level_of(p, rule->skill), rule->low,
                                     rule->high, woodcutting ? tool->scale : 100);
    if (!roll(world, chance)) return;
    char noun[48];
    item_noun((int)rule->product, noun, sizeof(noun));
    if (!rc_item_result_accepted(
            rc_player_inventory_add(world, (int)rule->product, 1, 0))) {
        sayf(p, "Your inventory is too full to hold any more %s.", noun, 0);
        rc_skilling_stop(p);
        return;
    }
    award_xp(p, rule->skill, rule->xp_tenths);
    sayf(p, woodcutting ? "You get some %s." : "You manage to mine some %s.",
         noun, 0);
    if (rule->deplete_one_in <= 1
            || rc_rng_bounded(&world->rng_state, rule->deplete_one_in) == 0) {
        deplete_node(world, node);
        rc_skilling_stop(p);
    } else if (inventory_full(p)) {
        sayf(p, "Your inventory is too full to hold any more %s.", noun, 0);
        rc_skilling_stop(p);
    }
}

// ---- Fishing ---------------------------------------------------------------

static const RcNpc *fishing_spot(const RcWorld *world, const RcPlayer *p,
                                 int uid) {
    const RcNpc *npc = rc_npc_resolve_const(world, (RcNpcId)uid);
    if (!npc || npc->is_dead || npc->plane != p->plane) return NULL;
    int dx = npc->x - p->x, dy = npc->y - p->y;
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1) return NULL;
    return npc;
}

static int fish_gear_ok(RcPlayer *p, const RcFishRule *rule) {
    char noun[48];
    if (!has_item(p, (int)rule->tool)) {
        sayf(p, "You need a %s to catch these fish.",
             item_noun((int)rule->tool, noun, sizeof(noun)), 0);
        return 0;
    }
    if (rule->bait && !has_item(p, (int)rule->bait)) {
        sayf(p, "You don't have any %s left.",
             item_noun((int)rule->bait, noun, sizeof(noun)), 0);
        return 0;
    }
    if (inventory_full(p)) {
        say(p, "You can't carry any more fish.");
        return 0;
    }
    return 1;
}

int rc_skilling_start_fish(RcWorld *world, int npc_uid, int option) {
    RcPlayer *p = world ? world->player : NULL;
    const RcNpc *npc = p ? rc_npc_resolve_const(world, (RcNpcId)npc_uid) : NULL;
    const RcNpcDef *def = npc ? rc_npc_base_def_for_npc(npc) : NULL;
    const RcFishRule *rule = def ? rc_skilling_fish_rule(world, def->id, option)
                                 : NULL;
    if (!rule) return 0;
    const RcFishCatch *lowest = &rule->catches[rule->catch_count - 1];
    if (level_of(p, SKILL_FISHING) < lowest->level) {
        sayf(p, "You need a Fishing level of at least %d to fish here.",
             NULL, lowest->level);
        return 0;
    }
    if (!fish_gear_ok(p, rule)) return 0;
    begin_job(world, RC_SKILL_JOB_FISH, (int)(rule - world->skilling->fish),
              world->skilling->fish_ticks);
    p->skill_job.target = npc_uid;
    say(p, "You attempt to catch some fish.");
    return 1;
}

static void fish_attempt(RcWorld *world, RcPlayer *p) {
    const RcSkillingRules *r = world->skilling;
    const RcFishRule *rule = &r->fish[p->skill_job.rule];
    if (!fishing_spot(world, p, p->skill_job.target) || !fish_gear_ok(p, rule)) {
        rc_skilling_stop(p);
        return;
    }
    p->skill_ready_tick = world->tick + r->fish_ticks;
    int level = level_of(p, SKILL_FISHING);
    for (int i = 0; i < rule->catch_count; i++) {
        const RcFishCatch *c = &rule->catches[i];
        if (level < c->level
                || !roll(world, rc_skill_chance_256(level, c->low, c->high, 100)))
            continue;
        if (!rc_item_result_accepted(
                rc_player_inventory_add(world, (int)c->product, 1, 0))) {
            rc_skilling_stop(p);
            return;
        }
        if (rule->bait)
            (void)rc_player_inventory_remove_item(world, (int)rule->bait, 1);
        award_xp(p, SKILL_FISHING, c->xp_tenths);
        char noun[48];
        const char *name = item_noun((int)c->product, noun, sizeof(noun));
        if (strncmp(name, "raw ", 4) == 0) name += 4;
        sayf(p, "You catch some %s.", name, 0);
        return;
    }
}

// ---- Firemaking ------------------------------------------------------------

int rc_skilling_start_firemaking(RcWorld *world, int inv_slot) {
    if (rc_player_command_should_queue(world)) {
        if (inv_slot < 0 || inv_slot >= RC_INVENTORY_SIZE) return 0;
        int args[8] = {inv_slot,
                       (int)world->player->inventory[inv_slot].generation,
                       0, 0, 0, 0, 0, 0};
        return rc_player_command_submit(world, RC_PLAYER_COMMAND_LIGHT_FIRE,
                                        RC_ACTION_CATEGORY_NORMAL, args, 0);
    }
    const RcSkillingRules *r = rules_of(world);
    RcPlayer *p = world ? world->player : NULL;
    if (!r || !p || inv_slot < 0 || inv_slot >= RC_INVENTORY_SIZE) return 0;
    int logs = p->inventory[inv_slot].item_id;
    const RcFireRule *rule = rc_skilling_fire_rule(world, logs);
    if (!rule) return 0;
    char noun[48];
    if (!has_item(p, (int)r->tinderbox)) {
        say(p, "You need a tinderbox to light a fire.");
        return 0;
    }
    if (level_of(p, SKILL_FIREMAKING) < rule->level) {
        char buf[RC_INTERACTION_RESULT_MESSAGE_LEN];
        snprintf(buf, sizeof(buf), "You need a Firemaking level of %d to burn %s.",
                 rule->level, item_noun(logs, noun, sizeof(noun)));
        say(p, buf);
        return 0;
    }
    RcObjectPlacement existing;
    if (rc_object_placement_find_layer(p->x, p->y, p->plane,
                                       rc_object_layer_for_type(10),
                                       &existing) != 0) {
        say(p, "You can't light a fire here.");
        return 0;
    }
    begin_job(world, RC_SKILL_JOB_FIREMAKING, (int)(rule - r->fires),
              r->fire_ticks);
    p->skill_job.item_id = logs;
    p->skill_job.x = (int16_t)p->x;
    p->skill_job.y = (int16_t)p->y;
    p->skill_job.plane = (int8_t)p->plane;
    say(p, "You attempt to light the logs.");
    return 1;
}

// After lighting a fire the player steps west, or east, south or north when
// west is blocked.
static void step_off_fire(RcWorld *world, RcPlayer *p) {
    static const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        if (rc_can_move(&world->map, p->x, p->y, dirs[i][0], dirs[i][1],
                        p->plane)) {
            (void)rc_player_walk_to(world, p->x + dirs[i][0],
                                    p->y + dirs[i][1]);
            return;
        }
    }
}

static void firemaking_attempt(RcWorld *world, RcPlayer *p) {
    const RcSkillingRules *r = world->skilling;
    RcSkillJob *job = &p->skill_job;
    const RcFireRule *rule = &r->fires[job->rule];
    int slot = rc_inv_find(p->inventory, job->item_id);
    if (slot < 0 || !has_item(p, (int)r->tinderbox) || p->x != job->x
            || p->y != job->y || p->plane != job->plane) {
        rc_skilling_stop(p);
        return;
    }
    p->skill_ready_tick = world->tick + r->fire_ticks;
    int chance = rc_skill_chance_256(level_of(p, SKILL_FIREMAKING),
                                     r->fire_low, r->fire_high, 100);
    if (!roll(world, chance)) return;
    RcObjectPlacement fire = {
        .obj_id = r->fire_obj_id, .x = (uint16_t)p->x, .y = (uint16_t)p->y,
        .plane = (uint8_t)p->plane, .type = 10,
    };
    int span = r->fire_max_ticks > r->fire_min_ticks
             ? r->fire_max_ticks - r->fire_min_ticks : 0;
    RcTick burn = r->fire_min_ticks + (RcTick)rc_rng_range(&world->rng_state, span);
    uint64_t key = 0;
    if (rc_world_object_add(world, &fire, burn, &key) != RC_OBJECT_MUTATION_OK) {
        say(p, "You can't light a fire here.");
        rc_skilling_stop(p);
        return;
    }
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, world).code == RC_ITEM_RESULT_OK
            && rc_item_tx_remove_slot(&tx, slot, 1,
                                      p->inventory[slot].generation).code
                == RC_ITEM_RESULT_OK) {
        (void)rc_item_tx_commit(&tx);
    }
    award_xp(p, SKILL_FIREMAKING, rule->xp_tenths);
    say(p, "The fire catches and the logs begin to burn.");
    rc_skilling_stop(p);
    step_off_fire(world, p);
}

// ---- Cooking ---------------------------------------------------------------

static int first_cookable(const RcWorld *world, const RcPlayer *p) {
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        if (p->inventory[i].item_id >= 0
                && rc_skilling_cook_rule(world, p->inventory[i].item_id))
            return p->inventory[i].item_id;
    return -1;
}

int rc_skilling_start_cook(RcWorld *world, int obj_id, int x, int y,
                           int plane, uint64_t placement_key, int raw_item) {
    RcPlayer *p = world ? world->player : NULL;
    if (!p || !rc_skilling_is_cook_object(world, obj_id)) return 0;
    if (raw_item < 0) raw_item = first_cookable(world, p);
    const RcCookRule *rule = rc_skilling_cook_rule(world, raw_item);
    if (!rule) {
        say(p, raw_item < 0 ? "You have nothing to cook."
                            : "You can't cook that.");
        return 0;
    }
    if (level_of(p, SKILL_COOKING) < rule->level) {
        sayf(p, "You need a Cooking level of %d to cook this.", NULL,
             rule->level);
        return 0;
    }
    begin_job(world, RC_SKILL_JOB_COOK, (int)(rule - world->skilling->cook), 1);
    p->skill_job.target = obj_id;
    p->skill_job.item_id = raw_item;
    p->skill_job.placement_key = placement_key;
    p->skill_job.x = (int16_t)x;
    p->skill_job.y = (int16_t)y;
    p->skill_job.plane = (int8_t)plane;
    return 1;
}

static void cook_attempt(RcWorld *world, RcPlayer *p) {
    const RcSkillingRules *r = world->skilling;
    RcSkillJob *job = &p->skill_job;
    const RcCookRule *rule = &r->cook[job->rule];
    RcObjectPlacement current;
    if (!rc_world_object_current_placement(world, job->target, job->x, job->y,
                                           job->plane, job->placement_key,
                                           &current)) {
        say(p, "The fire has gone out.");
        rc_skilling_stop(p);
        return;
    }
    int slot = rc_inv_find(p->inventory, (int)rule->raw);
    if (slot < 0) {
        rc_skilling_stop(p);  // cooked them all
        return;
    }
    p->skill_ready_tick = world->tick + r->cook_ticks;
    int cooked = roll(world, rc_skill_chance_256(level_of(p, SKILL_COOKING),
                                                 rule->low, rule->high, 100));
    if (!replace_slot_item(world, slot, (int)(cooked ? rule->cooked
                                                     : rule->burnt))) {
        rc_skilling_stop(p);
        return;
    }
    char noun[48];
    const char *name = item_noun((int)rule->cooked, noun, sizeof(noun));
    if (cooked) {
        award_xp(p, SKILL_COOKING, rule->xp_tenths);
        sayf(p, "You successfully cook the %s.", name, 0);
    } else {
        sayf(p, "You accidentally burn the %s.", name, 0);
    }
}

// ---- Furnace and anvil -----------------------------------------------------

int rc_skilling_make_option_ready(const RcWorld *world, int option) {
    if (!rules_of(world) || option < 0 || option >= world->make_option_count)
        return 0;
    const RcMakeOption *o = &world->make_options[option];
    if (world->player->make_menu != o->facility) return 0;
    return rc_recipe_player_can_make(world, rc_recipe_get(o->recipe));
}

int rc_skilling_open_make(RcWorld *world, RcMakeMenu menu) {
    const RcSkillingRules *r = rules_of(world);
    if (!r || menu == RC_MAKE_MENU_NONE) return 0;
    RcPlayer *p = world->player;
    if (menu == RC_MAKE_MENU_ANVIL && !has_item(p, (int)r->hammer)) {
        say(p, "You need a hammer to work the metal with.");
        return 0;
    }
    p->make_menu = (uint8_t)menu;
    for (int i = 0; i < world->make_option_count; i++)
        if (rc_skilling_make_option_ready(world, i)) return 1;
    p->make_menu = RC_MAKE_MENU_NONE;
    say(p, menu == RC_MAKE_MENU_FURNACE
        ? "You don't have the ores to smelt anything."
        : "You don't have enough bars to make anything.");
    return 0;
}

int rc_skilling_make(RcWorld *world, int option, int count) {
    if (rc_player_command_should_queue(world)) {
        int args[8] = {option, count, 0, 0, 0, 0, 0, 0};
        return rc_player_command_submit(world, RC_PLAYER_COMMAND_SKILL_MAKE,
                                        RC_ACTION_CATEGORY_SOFT, args, 0);
    }
    if (!rc_skilling_make_option_ready(world, option)) return 0;
    begin_job(world, RC_SKILL_JOB_MAKE, option, 1);
    world->player->skill_job.remaining = count > 0 ? count : -1;
    return 1;
}

static void make_attempt(RcWorld *world, RcPlayer *p) {
    RcSkillJob *job = &p->skill_job;
    const RcMakeOption *o = &world->make_options[job->rule];
    const RcRecipe *recipe = rc_recipe_get(o->recipe);
    if (!recipe || !rc_recipe_player_can_make(world, recipe)) {
        rc_skilling_stop(p);
        return;
    }
    p->skill_ready_tick = world->tick + (recipe->ticks ? recipe->ticks : 4);
    int made = o->success_pct >= 100
            || (int)rc_rng_bounded(&world->rng_state, 100) < o->success_pct;
    if (!rc_recipe_commit(world, recipe, made)) {
        rc_skilling_stop(p);
        return;
    }
    char noun[48];
    if (made) {
        for (int i = 0; i < recipe->req_count; i++)
            if (recipe->reqs[i].skill < SKILL_COUNT)
                award_xp(p, recipe->reqs[i].skill, recipe->reqs[i].xp_q1);
        sayf(p, o->facility == RC_MAKE_MENU_FURNACE
                ? "You retrieve a %s." : "You make a %s.",
             item_noun((int)recipe->output_item, noun, sizeof(noun)), 0);
    } else {
        say(p, "The ore is too impure and you fail to refine it.");
    }
    if (job->remaining > 0 && --job->remaining == 0) rc_skilling_stop(p);
}

// ---- Tick --------------------------------------------------------------------

void rc_skilling_tick(RcWorld *world) {
    RcPlayer *p = world ? world->player : NULL;
    if (!p) return;
    if (!p->skill_action || !rules_of(world)
            || p->skill_job.kind == RC_SKILL_JOB_NONE) {
        if (p->skill_job.kind != RC_SKILL_JOB_NONE) rc_skilling_stop(p);
        else if (p->skill_action && p->skill_ready_tick <= world->tick)
            p->skill_action = 0;  // a timed action with no job (recipes)
        return;
    }
    if (p->skill_ready_tick > world->tick) return;
    if (p->is_dead || p->current_hp <= 0) {
        rc_skilling_stop(p);
        return;
    }
    switch ((RcSkillJobKind)p->skill_job.kind) {
    case RC_SKILL_JOB_GATHER:     gather_attempt(world, p); break;
    case RC_SKILL_JOB_FISH:       fish_attempt(world, p); break;
    case RC_SKILL_JOB_FIREMAKING: firemaking_attempt(world, p); break;
    case RC_SKILL_JOB_COOK:       cook_attempt(world, p); break;
    case RC_SKILL_JOB_MAKE:       make_attempt(world, p); break;
    default:                      rc_skilling_stop(p); break;
    }
}
