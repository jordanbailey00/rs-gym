// Gym skilling content: OSRS tools, rates and products for woodcutting,
// mining, fishing, firemaking, cooking, smelting and smithing, and the
// object/NPC/item handlers that start them (engine: rc-core/skilling.c).
//
// Success values are OSRS skilling-chance low/high pairs (out of 256, in
// tenths), from the wiki's success-rate charts; mining and firemaking values
// are approximations.
#include "content.h"
#include "config.h"
#include "interaction.h"
#include "items.h"
#include "npc.h"
#include "skilling.h"
#include "skills.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum {
    TINDERBOX = 590, HAMMER = 2347, FIRE = 43475,
    LOGS = 1511, OAK_LOGS = 1521, WILLOW_LOGS = 1519, MAPLE_LOGS = 1517,
    YEW_LOGS = 1515,
    BRONZE_BAR = 2349, IRON_BAR = 2351, STEEL_BAR = 2353, MITHRIL_BAR = 2359,
    SMALL_NET = 303, FISHING_ROD = 307, FLY_ROD = 309, HARPOON = 311,
    LOBSTER_POT = 301, FISHING_BAIT = 313, FEATHER = 314,
    NET_SPOT = 1525, ROD_SPOT = 1526, CAGE_SPOT = 1519,
    FURNACE = 24009, ANVIL = 2097, RUSTED_ANVIL = 39620,
};

// Axes: woodcutting level, -, success scale. Pickaxes: mining level, ticks
// per swing. Best first.
static const RcSkillTool axes[] = {
    {6739, 61, 0, 375}, {1359, 41, 0, 350}, {1357, 31, 0, 300},
    {1355, 21, 0, 250}, {1361, 11, 0, 225}, {1353, 6, 0, 200},
    {1349, 1, 0, 150},  {1351, 1, 0, 100},
};
static const RcSkillTool pickaxes[] = {
    {11920, 61, 3, 0}, {1275, 41, 3, 0}, {1271, 31, 4, 0}, {1273, 21, 5, 0},
    {12297, 11, 5, 0}, {1269, 6, 6, 0},  {1267, 1, 7, 0},  {1265, 1, 8, 0},
};

// Trees deplete after one log (normal trees) or with chance 1/8 per log.
// Rocks deplete after every ore.
#define TREE(id, logs, lvl, xp, lo, hi, dep) \
    {id, logs, SKILL_WOODCUTTING, lvl, xp, lo, hi, dep}
#define ROCK(id, ore, lvl, xp, lo, hi) \
    {id, ore, SKILL_MINING, lvl, xp, lo, hi, 1}
static const RcGatherRule gather[] = {
    TREE(1276, LOGS, 1, 250, 640, 2000, 1),
    TREE(1278, LOGS, 1, 250, 640, 2000, 1),
    TREE(10820, OAK_LOGS, 15, 375, 320, 1000, 8),
    TREE(10819, WILLOW_LOGS, 30, 675, 160, 500, 8),
    TREE(10829, WILLOW_LOGS, 30, 675, 160, 500, 8),
    TREE(10831, WILLOW_LOGS, 30, 675, 160, 500, 8),
    TREE(10833, WILLOW_LOGS, 30, 675, 160, 500, 8),
    TREE(10832, MAPLE_LOGS, 45, 1000, 80, 250, 8),
    TREE(10822, YEW_LOGS, 60, 1750, 40, 125, 8),
    ROCK(10943, 436, 1, 175, 1000, 3500), ROCK(11161, 436, 1, 175, 1000, 3500),
    ROCK(11360, 438, 1, 175, 1000, 3500), ROCK(11361, 438, 1, 175, 1000, 3500),
    ROCK(11364, 440, 15, 350, 960, 3500), ROCK(11365, 440, 15, 350, 960, 3500),
    ROCK(11366, 453, 30, 500, 160, 1000), ROCK(11367, 453, 30, 500, 160, 1000),
    ROCK(11372, 447, 55, 800, 80, 500),   ROCK(11373, 447, 55, 800, 80, 500),
};

// Fishing spot options; catches are rolled highest level first.
static const RcFishRule fish[] = {
    {NET_SPOT, 0, 2, SMALL_NET, 0,
     {{321, 15, 400, 240, 1280}, {317, 1, 100, 480, 2560}}},       // anchovies, shrimps
    {NET_SPOT, 2, 2, FISHING_ROD, FISHING_BAIT,
     {{345, 10, 300, 240, 1280}, {327, 5, 200, 320, 1920}}},       // herring, sardine
    {ROD_SPOT, 0, 2, FLY_ROD, FEATHER,
     {{331, 30, 700, 160, 960}, {335, 20, 500, 320, 1920}}},       // salmon, trout
    {ROD_SPOT, 2, 1, FISHING_ROD, FISHING_BAIT,
     {{349, 25, 600, 160, 960}}},                                  // pike
    {CAGE_SPOT, 0, 1, LOBSTER_POT, 0, {{377, 40, 900, 60, 950}}},  // lobster
    {CAGE_SPOT, 2, 2, HARPOON, 0,
     {{371, 50, 1000, 40, 480}, {359, 35, 800, 80, 640}}},         // swordfish, tuna
};

static const RcFireRule fires[] = {
    {LOGS, 1, 400}, {OAK_LOGS, 15, 600}, {WILLOW_LOGS, 30, 900},
    {MAPLE_LOGS, 45, 1350}, {YEW_LOGS, 60, 2025},
};

static const RcCookRule cook[] = {
    {317, 315, 7954, 1, 300, 1280, 5120},   // shrimps
    {321, 319, 323, 1, 300, 1280, 5120},    // anchovies
    {327, 325, 369, 1, 400, 1180, 4920},    // sardine
    {345, 347, 357, 5, 500, 1080, 4720},    // herring
    {335, 333, 343, 15, 700, 880, 4320},    // trout
    {349, 351, 343, 20, 800, 780, 4120},    // pike
    {331, 329, 343, 25, 900, 680, 3920},    // salmon
    {359, 361, 367, 30, 1000, 580, 3720},   // tuna
    {377, 379, 381, 40, 1200, 380, 3320},   // lobster
    {371, 373, 375, 45, 1400, 180, 2920},   // swordfish
    {2132, 2142, 2146, 1, 300, 1280, 5120}, // raw beef
    {2134, 2142, 2146, 1, 300, 1280, 5120}, // raw rat meat
    {2138, 2140, 2144, 1, 300, 1280, 5120}, // raw chicken
};

// Fires and ranges with a Cook option (the gym's fires are loc 43475).
static const uint32_t cook_objects[] = {FIRE, 114, 9682, 26181, 21792, 7183,
                                        7184, 12269};

static const RcSkillingRules rules = {
    .axes = axes, .axe_count = COUNT(axes),
    .pickaxes = pickaxes, .pickaxe_count = COUNT(pickaxes),
    .gather = gather, .gather_count = COUNT(gather),
    .fish = fish, .fish_count = COUNT(fish),
    .fires = fires, .fire_count = COUNT(fires),
    .cook = cook, .cook_count = COUNT(cook),
    .cook_objects = cook_objects, .cook_object_count = COUNT(cook_objects),
    .tinderbox = TINDERBOX, .hammer = HAMMER, .fire_obj_id = FIRE,
    .woodcut_ticks = 4, .fish_ticks = 5, .fire_ticks = 4, .cook_ticks = 4,
    .fire_low = 650, .fire_high = 5130,
    .fire_min_ticks = 100, .fire_max_ticks = 200,
};

// ---- Furnace and anvil products from recipes.bin --------------------------

static const char *const metals[] = {"Bronze", "Iron", "Steel", "Mithril"};
static const int bars[] = {BRONZE_BAR, IRON_BAR, STEEL_BAR, MITHRIL_BAR};
// Smithable items with a use in the gym (no fletching parts).
static const char *const smithable[] = {
    "dagger", "axe", "mace", "med helm", "sword", "scimitar", "longsword",
    "full helm", "sq shield", "warhammer", "battleaxe", "chainbody",
    "kiteshield", "2h sword", "platelegs", "plateskirt", "platebody",
};

typedef struct { int recipe, facility, metal, level; } MakeCandidate;

static int metal_of_bar(int item_id) {
    for (int i = 0; i < COUNT(bars); i++) if (bars[i] == item_id) return i;
    return -1;
}

static int smithable_name(const char *name, int metal) {
    size_t n = strlen(metals[metal]);
    if (strncmp(name, metals[metal], n) != 0 || name[n] != ' ') return 0;
    for (int i = 0; i < COUNT(smithable); i++)
        if (strcmp(name + n + 1, smithable[i]) == 0) return 1;
    return 0;
}

static int compare_candidates(const void *a, const void *b) {
    const MakeCandidate *x = a, *y = b;
    if (x->facility != y->facility) return x->facility - y->facility;
    if (x->metal != y->metal) return x->metal - y->metal;
    if (x->level != y->level) return x->level - y->level;
    return x->recipe - y->recipe;
}

static void add_make_options(RcWorld *world) {
    MakeCandidate found[RC_MAKE_MAX_OPTIONS];
    int count = 0;
    const RcRecipe *recipe;
    for (int i = 0; (recipe = rc_recipe_get(i)) != NULL; i++) {
        if (recipe->flags || recipe->req_count < 1 || count >= COUNT(found))
            continue;
        int facility = 0, metal = -1;
        if (strcmp(recipe->facility, "Furnace") == 0) {
            metal = metal_of_bar((int)recipe->output_item);
            facility = RC_MAKE_MENU_FURNACE;
        } else if (strcmp(recipe->facility, "Anvil") == 0
                && recipe->input_count == 1) {
            metal = metal_of_bar((int)recipe->inputs[0].item_id);
            if (metal >= 0 && !smithable_name(recipe->name, metal)) metal = -1;
            facility = RC_MAKE_MENU_ANVIL;
        }
        if (metal < 0) continue;
        found[count++] = (MakeCandidate){i, facility, metal,
                                         recipe->reqs[0].level};
    }
    qsort(found, (size_t)count, sizeof(found[0]), compare_candidates);
    for (int i = 0; i < count; i++) {
        // An ordinary furnace refines iron ore half the time.
        int pct = found[i].facility == RC_MAKE_MENU_FURNACE
                       && bars[found[i].metal] == IRON_BAR ? 50 : 100;
        (void)rc_skilling_add_make_option(world, found[i].recipe,
                                          (RcMakeMenu)found[i].facility, pct);
    }
}

// ---- Handlers -----------------------------------------------------------------

static RcInteractionHandlerResult fish_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending,
    void *ctx) {
    const RcFishRule *rule = ctx;
    uint64_t said = player->interaction_outcome.sequence;
    int ok = rc_skilling_start_fish(world, pending->target.entity_uid,
                                    rule->option);
    return rc_interaction_result_after(player, said, ok);
}

static RcInteractionHandlerResult cook_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending,
    void *ctx) {
    (void)ctx;
    uint64_t said = player->interaction_outcome.sequence;
    int raw = pending->op == RC_INTERACTION_USE_ON ? pending->source_item_id : -1;
    int ok = rc_skilling_start_cook(world, pending->target.definition_id,
                                    pending->target.tile_x,
                                    pending->target.tile_y,
                                    pending->target.plane,
                                    pending->target.placement_key, raw);
    return rc_interaction_result_after(player, said, ok);
}

static RcInteractionHandlerResult make_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending,
    void *ctx) {
    (void)pending;
    uint64_t said = player->interaction_outcome.sequence;
    int ok = rc_skilling_open_make(world, (RcMakeMenu)(intptr_t)ctx);
    return rc_interaction_result_after(player, said, ok);
}

// Tinderbox on logs, or logs on a tinderbox.
static RcInteractionHandlerResult light_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending,
    void *ctx) {
    (void)ctx;
    uint64_t said = player->interaction_outcome.sequence;
    int slot = pending->source_item_id == TINDERBOX
             ? pending->target.inventory_slot : pending->source_inventory_slot;
    int ok = rc_skilling_start_firemaking(world, slot);
    return rc_interaction_result_after(player, said, ok);
}

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

int rc_content_skilling_register(RcWorld *world) {
    if (!world) return 0;
    if (!(world->enabled & RC_SUB_SKILLS)) return 1;
    rc_skilling_install(world, &rules);
    add_make_options(world);
    int ok = 1;
    for (int i = 0; i < COUNT(fish); i++)
        ok &= handle(world, RC_INTERACTION_NPC,
                     rc_interaction_op_from_option(fish[i].option),
                     (int)fish[i].npc_id, RC_INTERACTION_KEY_ANY,
                     fish_handler, (void *)&fish[i]);
    for (int i = 0; i < COUNT(cook_objects); i++) {
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_OP1,
                     (int)cook_objects[i], RC_INTERACTION_KEY_ANY,
                     cook_handler, NULL);
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_USE_ON,
                     (int)cook_objects[i], RC_INTERACTION_KEY_ANY,
                     cook_handler, NULL);
    }
    void *furnace = (void *)(intptr_t)RC_MAKE_MENU_FURNACE;
    void *anvil = (void *)(intptr_t)RC_MAKE_MENU_ANVIL;
    ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_OP2, FURNACE,
                 RC_INTERACTION_KEY_ANY, make_handler, furnace);
    ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_USE_ON, FURNACE,
                 RC_INTERACTION_KEY_ANY, make_handler, furnace);
    for (int i = 0; i < 2; i++) {
        int id = i ? RUSTED_ANVIL : ANVIL;
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_OP1, id,
                     RC_INTERACTION_KEY_ANY, make_handler, anvil);
        ok &= handle(world, RC_INTERACTION_OBJECT, RC_INTERACTION_USE_ON, id,
                     RC_INTERACTION_KEY_ANY, make_handler, anvil);
    }
    for (int i = 0; i < COUNT(fires); i++) {
        ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_USE_ON,
                     (int)fires[i].logs, TINDERBOX, light_handler, NULL);
        ok &= handle(world, RC_INTERACTION_INVENTORY_ITEM, RC_INTERACTION_USE_ON,
                     TINDERBOX, (int)fires[i].logs, light_handler, NULL);
    }
    return ok;
}
