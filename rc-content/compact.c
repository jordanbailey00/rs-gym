// A gym mode's content from its compiled rules (rc-core/mode.h, data/modes/<mode>/mode.rules): the
// skilling rules and production stations, fixed-price shops, the tanner and Historian Aldo, burying
// bones, the closed drop tables, death and loot timing, and the fresh-character kit. Installed only
// when the loaded game data has mode rules; the island mode uses skilling.c and gym.c instead.
#include "content.h"
#include "config.h"
#include "death.h"
#include "drops.h"
#include "interaction.h"
#include "items.h"
#include "mode.h"
#include "npc.h"
#include "rng.h"
#include "shops.h"
#include "skilling.h"
#include "skills.h"
#include "storage.h"

#include <stdint.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { HAMMER = 2347, COINS = 995 };

// ---- Skilling rules ------------------------------------------------------------------------------
// Built once per process from the immutable rules; every world shares them.

static RcSkillingRules skilling_rules;
static const RcModeRules *skilling_rules_for;

static const RcSkillingRules *mode_skilling(const RcModeRules *m) {
    if (skilling_rules_for == m) return &skilling_rules;
    skilling_rules = (RcSkillingRules){
        .axes = m->axes, .axe_count = m->axe_count,
        .pickaxes = m->pickaxes, .pickaxe_count = m->pickaxe_count,
        .gather = m->gather, .gather_count = m->gather_count,
        .fish = m->fish, .fish_count = m->fish_count,
        .cook = m->cook, .cook_count = m->cook_count,
        .cook_objects = m->cook_objects, .cook_object_count = m->cook_object_count,
        .hammer = HAMMER,
        .woodcut_ticks = 4, .fish_ticks = 5, .cook_ticks = 4,
    };
    skilling_rules_for = m;
    return &skilling_rules;
}

// ---- Handlers ------------------------------------------------------------------------------------

static int handle(RcWorld *world, RcInteractionKind kind, RcInteractionOp op,
                  int definition_id, int source_item_id,
                  RcInteractionHandlerFn fn, void *ctx) {
    RcInteractionDispatchKey key = rc_interaction_dispatch_key_any();
    key.kind = kind;
    key.op = op;
    key.definition_id = definition_id;
    key.source_item_id = source_item_id;
    return rc_interaction_register_world_handler(world, &key, fn, ctx);
}

static RcInteractionHandlerResult fish_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending, void *ctx) {
    const RcFishRule *rule = ctx;
    uint64_t said = player->interaction_outcome.sequence;
    int ok = rc_skilling_start_fish(world, pending->target.entity_uid, rule->option);
    return rc_interaction_result_after(player, said, ok);
}

static RcInteractionHandlerResult cook_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending, void *ctx) {
    (void)ctx;
    uint64_t said = player->interaction_outcome.sequence;
    int raw = pending->op == RC_INTERACTION_USE_ON ? pending->source_item_id : -1;
    int ok = rc_skilling_start_cook(world, pending->target.definition_id, pending->target.tile_x,
                                    pending->target.tile_y, pending->target.plane,
                                    pending->target.placement_key, raw);
    return rc_interaction_result_after(player, said, ok);
}

static RcInteractionHandlerResult gather_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending, void *ctx) {
    (void)ctx;
    uint64_t said = player->interaction_outcome.sequence;
    int ok = rc_skilling_start_gather(world, pending->target.definition_id, pending->target.tile_x,
                                      pending->target.tile_y, pending->target.plane);
    return rc_interaction_result_after(player, said, ok);
}

// A station object or a tool used on an item opens that station's menu.
static RcInteractionHandlerResult station_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending, void *ctx) {
    uint64_t said = player->interaction_outcome.sequence;
    RcMakeMenu menu = (RcMakeMenu)(intptr_t)ctx;
    int loc = pending->target.kind == RC_INTERACTION_OBJECT ? pending->target.definition_id : 0;
    int ok = rc_skilling_open_station(world, menu, menu == RC_MAKE_MENU_ALTAR ? loc : 0);
    if (ok && menu == RC_MAKE_MENU_ALTAR) {
        // Craft-rune binds every carried essence at once: start the altar's only product.
        for (int i = 0; i < world->make_option_count; i++) {
            if (rc_skilling_make_option_ready(world, i)) {
                ok = rc_skilling_make(world, i, 0);
                break;
            }
        }
    }
    return rc_interaction_result_after(player, said, ok);
}

static RcInteractionHandlerResult trade_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending, void *ctx) {
    (void)player;
    (void)pending;
    if (!rc_shop_open(world, (int)(intptr_t)ctx))
        return rc_interaction_result_failure(RC_INTERACTION_FAIL_INVALID_TARGET,
                                             "The shop is closed.");
    return rc_interaction_result_complete();
}

static RcInteractionHandlerResult bury_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending, void *ctx) {
    int slot = pending->target.inventory_slot;
    if (slot < 0 || slot >= RC_INVENTORY_SIZE
            || player->inventory[slot].item_id != pending->target.definition_id)
        return rc_interaction_result_cancel(RC_INTERACTION_FAIL_TARGET_MISSING);
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, world).code != RC_ITEM_RESULT_OK
            || rc_item_tx_remove_slot(&tx, slot, 1, player->inventory[slot].generation).code
                != RC_ITEM_RESULT_OK
            || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK)
        return rc_interaction_result_cancel(RC_INTERACTION_FAIL_TARGET_MISSING);
    rc_add_xp_hundredths(&player->skills, SKILL_PRAYER, (int64_t)(intptr_t)ctx * 10);
    return rc_interaction_result_message("You bury the bones.");
}

// The NPC option index named `name` ("Trade"), or -1.
static int npc_option(int npc_id, const char *name) {
    int count = 0;
    const RcNpcDef *defs = rc_npc_defs_all(&count);
    for (int i = 0; defs && i < count; i++) {
        if (defs[i].id != npc_id) continue;
        for (int o = 0; o < RC_NPC_OPTION_COUNT; o++)
            if (!strcmp(defs[i].options[o], name)) return o;
        return -1;
    }
    return -1;
}

static int register_npc_option(RcWorld *world, int npc_id, const char *option,
                               RcInteractionHandlerFn fn, void *ctx) {
    int o = npc_option(npc_id, option);
    return o >= 0 && handle(world, RC_INTERACTION_NPC, rc_interaction_op_from_option(o), npc_id,
                            RC_INTERACTION_KEY_ANY, fn, ctx);
}

// Item-on-item combinations that open a menu: a tool on each input, or two inputs together.
static int register_item_pairs(RcWorld *world, const RcModeRules *m) {
    static const struct { RcMakeMenu menu; int tool; } tool_menus[] = {
        {RC_MAKE_MENU_KNIFE, 946}, {RC_MAKE_MENU_NEEDLE, 1733}, {RC_MAKE_MENU_CHISEL, 1755},
    };
    int ok = 1;
    for (int i = 0; i < m->recipe_count; i++) {
        const RcModeRecipe *x = &m->recipes[i];
        void *ctx = (void *)(intptr_t)x->station;
        int tool = 0;
        for (int t = 0; t < COUNT(tool_menus); t++)
            if (tool_menus[t].menu == x->station) tool = tool_menus[t].tool;
        if (tool) {
            for (int j = 0; j < x->recipe.input_count; j++) {
                int item = (int)x->recipe.inputs[j].item_id;
                ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_USE_ON, item, tool,
                             station_handler, ctx);
                ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_USE_ON, tool, item,
                             station_handler, ctx);
            }
        } else if (x->station == RC_MAKE_MENU_COMBINE && x->recipe.input_count >= 2) {
            int a = (int)x->recipe.inputs[0].item_id, b = (int)x->recipe.inputs[1].item_id;
            ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_USE_ON, a, b,
                         station_handler, ctx);
            ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_USE_ON, b, a,
                         station_handler, ctx);
        }
    }
    return ok;
}

static int register_stations(RcWorld *world, const RcModeRules *m) {
    int ok = 1;
    for (int i = 0; i < m->station_count; i++) {
        void *ctx = (void *)(intptr_t)m->stations[i].station;
        int loc = m->stations[i].loc;
        // Furnaces smelt on their second option; anvils and spinning wheels on the first.
        RcInteractionOp op = m->stations[i].station == RC_MAKE_MENU_FURNACE
                           ? RC_INTERACTION_OP2 : RC_INTERACTION_OP1;
        ok &= handle(world, RC_INTERACTION_OBJECT, op, loc, RC_INTERACTION_KEY_ANY,
                     station_handler, ctx);
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_USE_ON, loc,
                     RC_INTERACTION_KEY_ANY, station_handler, ctx);
    }
    for (int i = 0; i < m->recipe_count; i++) {
        const RcModeRecipe *x = &m->recipes[i];
        if (x->station != RC_MAKE_MENU_ALTAR || !x->loc) continue;
        void *ctx = (void *)(intptr_t)RC_MAKE_MENU_ALTAR;
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_OP1, x->loc,
                     RC_INTERACTION_KEY_ANY, station_handler, ctx);
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_USE_ON, x->loc,
                     RC_INTERACTION_KEY_ANY, station_handler, ctx);
    }
    return ok;
}

// ---- Shops -----------------------------------------------------------------------------------------

static int sale_value(int item_id) {
    return rc_mode_sale_value(rc_mode_active(), item_id);
}

static int install_shops(RcWorld *world, const RcModeRules *m) {
    int ok = 1;
    world->shop_count = 0;
    for (int s = 0; s < m->shop_count; s++) {
        const RcModeShop *shop = &m->shops[s];
        RcShopItemDef items[RC_MODE_MAX_STOCK];
        for (int i = 0; i < shop->stock_count; i++)
            items[i] = (RcShopItemDef){.item_id = shop->stock[i].item, .stock = shop->stock[i].cap,
                .restock_ticks = (uint16_t)shop->stock[i].ticks, .price = shop->stock[i].price,
                .restock_qty = (uint16_t)shop->stock[i].qty};
        RcShopDef def = {.name = shop->name, .npc_id = shop->npc, .items = items,
                         .item_count = shop->stock_count, .fixed = true,
                         .buys_valued = shop->buys_valued, .sale_value = sale_value};
        int idx = rc_shop_install(world, &def);
        ok &= idx >= 0 && register_npc_option(world, shop->npc, "Trade", trade_handler,
                                              (void *)(intptr_t)idx);
    }
    return ok;
}

// ---- Drops ---------------------------------------------------------------------------------------

static int roll_quantity(RcWorld *world, const RcModeDropRow *row) {
    return row->max > row->min ? row->min + rc_rng_range(&world->rng_state, row->max - row->min)
                               : row->min;
}

static int add_grant(RcGroundGrant *out, int capacity, int *count, int item, int qty) {
    if (*count >= capacity || qty <= 0) return 0;
    out[(*count)++] = (RcGroundGrant){.item_id = item, .quantity = qty};
    return 1;
}

// The mode's closed tables replace every inherited one: always rows, then each independent
// 1-in-N roll picking one row by weight. NPCs without a mode table drop nothing.
static RcLootPolicy mode_loot(RcWorld *world, const RcNpc *npc, RcGroundGrant *out, int capacity,
                              int *count, void *ctx) {
    const RcModeRules *m = ctx;
    const RcNpcDef *def = rc_npc_def_for_npc(world, npc);
    const RcModeDrop *d = def ? rc_mode_drop(m, def->id) : NULL;
    *count = 0;
    if (!d) return RC_LOOT_SUPPRESS;
    for (int i = 0; i < d->always_count; i++)
        add_grant(out, capacity, count, d->always[i].item, roll_quantity(world, &d->always[i]));
    for (int r = 0; r < d->roll_count; r++) {
        const RcModeDropRoll *roll = &d->rolls[r];
        if (roll->one_in > 1 && rc_rng_range(&world->rng_state, roll->one_in - 1) != 0) continue;
        int total = 0;
        for (int i = 0; i < roll->count; i++) total += roll->pick[i].weight;
        int pick = total > 0 ? rc_rng_range(&world->rng_state, total - 1) : 0;
        for (int i = 0; i < roll->count; i++) {
            if (pick < roll->pick[i].weight) {
                add_grant(out, capacity, count, roll->pick[i].item,
                          roll_quantity(world, &roll->pick[i]));
                break;
            }
            pick -= roll->pick[i].weight;
        }
    }
    return RC_LOOT_REPLACE;
}

// ---- Fresh character -----------------------------------------------------------------------------

void rc_content_compact_new_character(RcWorld *world) {
    const RcModeRules *m = rc_mode_active();
    RcPlayer *p = world ? world->player : NULL;
    if (!m || !p) return;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        p->inventory[i] = (RcInvSlot){.item_id = -1, .generation = p->inventory[i].generation + 1};
    for (int i = 0; i < RC_EQUIP_COUNT; i++)
        p->equipment[i] = (RcInvSlot){.item_id = -1, .generation = p->equipment[i].generation + 1};
    for (int i = 0; i < RC_BANK_SIZE; i++) p->bank[i] = (RcInvSlot){.item_id = -1};
    for (int s = 0; s < SKILL_COUNT; s++) {
        int level = s == SKILL_HITPOINTS ? 10 : 1;
        p->skills.base_level[s] = p->skills.boosted_level[s] = level;
        p->skills.xp[s] = RC_XP_TABLE[level - 1];
        p->skills.xp_hundredths[s] = 0;
    }
    p->max_hp = p->current_hp = 10 * 10;  // hitpoints and prayer are kept in tenths
    p->current_prayer_points = 1 * 10;
    for (int i = 0; i < m->start_count[RC_MODE_START_EQUIP]; i++) {
        const RcModeStack *s = &m->start[RC_MODE_START_EQUIP][i];
        const RcItemDef *def = rc_item_def_get(s->item);
        if (def && def->equippable && def->equip_slot >= 0 && def->equip_slot < RC_EQUIP_COUNT)
            p->equipment[def->equip_slot] = (RcInvSlot){.item_id = s->item, .quantity = s->qty};
    }
    for (int i = 0; i < m->start_count[RC_MODE_START_INVENTORY]; i++) {
        const RcModeStack *s = &m->start[RC_MODE_START_INVENTORY][i];
        const RcItemDef *def = rc_item_def_get(s->item);
        int each = def && def->stackable ? s->qty : 1;
        for (int n = 0; n < s->qty; n += each)
            (void)rc_player_inventory_add(world, s->item, each, 0);
    }
    for (int i = 0; i < m->start_count[RC_MODE_START_BANK]; i++)
        (void)rc_bank_add_item(world, m->start[RC_MODE_START_BANK][i].item,
                               m->start[RC_MODE_START_BANK][i].qty);
    p->run_energy = 10000;
    p->special_energy = 10000;
    p->active_prayers = 0;
    rc_recalc_bonuses(p);
}

// ---- Registration --------------------------------------------------------------------------------

int rc_content_compact_register(RcWorld *world) {
    const RcModeRules *m = rc_mode_active();
    if (!world) return 0;
    if (!m) return 1;
    static RcDeathRules death;
    death = (RcDeathRules){.keep_items = 0, .respawn_delay = (uint16_t)m->respawn_ticks,
                           .grave_ticks = (uint16_t)m->recovery_ticks,
                           .wild_drop_ticks = (uint16_t)m->recovery_ticks};
    world->death_rules = &death;
    world->loot_private_ticks = m->private_ticks;
    world->loot_public_ticks = m->public_ticks;
    if (world->enabled & RC_SUB_LOOT) rc_set_npc_loot_hook(world, mode_loot, (void *)m);
    int ok = 1;
    if (world->enabled & RC_SUB_SKILLS) {
        rc_skilling_install(world, mode_skilling(m));
        for (int i = 0; i < m->recipe_count; i++)
            ok &= rc_skilling_add_station_option(world, i, (RcMakeMenu)m->recipes[i].station, 100,
                                                 m->recipes[i].batch, m->recipes[i].loc) >= 0;
        for (int i = 0; i < m->fish_count; i++)
            ok &= handle(world, RC_INTERACTION_NPC, rc_interaction_op_from_option(m->fish[i].option),
                         (int)m->fish[i].npc_id, RC_INTERACTION_KEY_ANY, fish_handler,
                         (void *)&m->fish[i]);
        for (int i = 0; i < m->cook_object_count; i++) {
            ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_OP1, (int)m->cook_objects[i],
                         RC_INTERACTION_KEY_ANY, cook_handler, NULL);
            ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_USE_ON, (int)m->cook_objects[i],
                         RC_INTERACTION_KEY_ANY, cook_handler, NULL);
        }
        // Hand-picked and never-depleting nodes have no resource behaviour of their own.
        for (int i = 0; i < m->gather_count; i++)
            if ((m->gather[i].flags & RC_GATHER_NO_TOOL) || !m->gather[i].deplete_one_in)
                ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_OP1, (int)m->gather[i].obj_id,
                             RC_INTERACTION_KEY_ANY, gather_handler, NULL);
        ok &= register_stations(world, m);
        ok &= register_item_pairs(world, m);
        if (m->tanner_npc)
            ok &= register_npc_option(world, m->tanner_npc, "Trade", station_handler,
                                      (void *)(intptr_t)RC_MAKE_MENU_TANNER);
        if (m->aldo_npc)
            ok &= register_npc_option(world, m->aldo_npc, "Trade", station_handler,
                                      (void *)(intptr_t)RC_MAKE_MENU_ALDO);
    }
    if (world->enabled & RC_SUB_INVENTORY) {
        for (int i = 0; i < m->bury_count; i++)
            ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_OP1, m->bury[i].item,
                         RC_INTERACTION_KEY_ANY, bury_handler, (void *)(intptr_t)m->bury[i].xp_tenths);
        ok &= install_shops(world, m);
    }
    return ok;
}
