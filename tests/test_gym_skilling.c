// Gym skilling end to end (run tools/build_gym.py first): chop a tree to a stump, mine a rock
// empty, net fish, light a fire and cook on it, smelt a bar and smith it.
#include "../rc-core/api.h"
#include "../rc-core/config.h"
#include "../rc-core/items.h"
#include "../rc-core/object_runtime.h"
#include "../rc-core/objects.h"
#include "../rc-core/pathfinding.h"
#include "../rc-core/skilling.h"
#include "../rc-core/skills.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum {
    SKIP = 77, BRONZE_AXE = 1351, BRONZE_PICKAXE = 1265, SMALL_NET = 303, TINDERBOX = 590,
    HAMMER = 2347, LOGS = 1511, COPPER_ORE = 436, TIN_ORE = 438, RAW_SHRIMPS = 317,
    SHRIMPS = 315, BRONZE_BAR = 2349, BRONZE_DAGGER = 1205, TREE = 1276, TREE_B = 1278,
    STUMP = 1342, FIRE = 43475, FURNACE = 24009, NET_SPOT = 1525,
};

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int count_item(const RcPlayer *p, int item) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        if (p->inventory[i].item_id == item) n += p->inventory[i].quantity;
    return n;
}

static void clear_inventory(RcWorld *w) {
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
        RcInvSlot *slot = &w->player->inventory[i];
        slot->item_id = -1;
        slot->quantity = 0;
        slot->state_id = 0;
        slot->generation++;
    }
}

static void give(RcWorld *w, int item, int qty) {
    for (int i = 0; i < qty; i++) rc_player_inventory_add(w, item, 1, 0);
}

// Ticks until cond holds (or the limit), returning whether it held.
#define TICK_UNTIL(w, limit, cond) ({ int _ok = 0; for (int _t = 0; _t < (limit); _t++) { \
    rc_world_tick(w); if (cond) { _ok = 1; break; } } _ok; })

// First placement of obj_id (or alt) anywhere in the gym's loaded area.
static int find_object(const RcWorld *w, int obj_id, int alt, RcObjectPlacement *out) {
    (void)w;
    for (int rx = gym_layout.min_rx; rx <= gym_layout.max_rx; rx++)
        for (int ry = gym_layout.min_ry; ry <= gym_layout.max_ry; ry++) {
            int count = 0;
            const RcObjectPlacement *rows = rc_object_region_placements((uint16_t)((rx << 8) | ry), &count);
            for (int i = 0; rows && i < count; i++)
                if ((int)rows[i].obj_id == obj_id || (int)rows[i].obj_id == alt) {
                    *out = rows[i];
                    return 1;
                }
        }
    return 0;
}

// Puts the player on a walkable tile within a few tiles of (x, y); the interaction walks the rest.
static int stand_near(RcWorld *w, int x, int y, int plane) {
    for (int r = 1; r <= 4; r++)
        for (int dx = -r; dx <= r; dx++)
            for (int dy = -r; dy <= r; dy++)
                if ((dx == -r || dx == r || dy == -r || dy == r)
                        && !rc_tile_blocked(&w->map, x + dx, y + dy, plane)
                        && rc_world_relocate_player(w, x + dx, y + dy, plane))
                    return 1;
    return 0;
}

static int option_index(const RcWorld *w, const RcObjectPlacement *o, const char *label) {
    for (int op = 0; op < RC_OBJECT_ACTIONS; op++)
        if (rc_world_object_option_supported(w, (int)o->obj_id, o->x, o->y, o->plane, 0, op)
                && strcmp(rc_world_object_option_label(w, (int)o->obj_id, o->x, o->y, o->plane, 0, op),
                          label) == 0)
            return op;
    return -1;
}

static int make_option_for(const RcWorld *w, int output) {
    for (int i = 0; i < w->make_option_count; i++)
        if ((int)rc_recipe_get(w->make_options[i].recipe)->output_item == output) return i;
    return -1;
}

static void woodcutting_and_mining(RcWorld *w) {
    RcPlayer *p = w->player;
    RcObjectPlacement tree;
    CHECK(find_object(w, TREE, TREE_B, &tree), "no tree in the gym");
    clear_inventory(w);
    give(w, BRONZE_AXE, 1);
    p->skills.base_level[SKILL_WOODCUTTING] = p->skills.boosted_level[SKILL_WOODCUTTING] = 99;
    CHECK(stand_near(w, tree.x, tree.y, tree.plane), "no tile near the tree");
    int op = option_index(w, &tree, "Chop down");
    CHECK(op >= 0, "tree has no Chop down option");
    rc_player_interact_object_at(w, (int)tree.obj_id, tree.x, tree.y, tree.plane, op);
    CHECK(TICK_UNTIL(w, 200, count_item(p, LOGS) > 0), "no logs after 200 ticks");
    CHECK(p->skills.xp[SKILL_WOODCUTTING] >= 25, "woodcutting xp %d", p->skills.xp[SKILL_WOODCUTTING]);
    CHECK(rc_world_object_active_id(w, (int)tree.obj_id, tree.x, tree.y, tree.plane) == STUMP,
          "tree did not become a stump (active %d)",
          rc_world_object_active_id(w, (int)tree.obj_id, tree.x, tree.y, tree.plane));
    CHECK(!p->skill_action, "still chopping a stump");

    RcObjectPlacement rock;
    CHECK(find_object(w, 10943, 11161, &rock), "no copper rock in the gym");
    give(w, BRONZE_PICKAXE, 1);
    p->skills.base_level[SKILL_MINING] = p->skills.boosted_level[SKILL_MINING] = 99;
    CHECK(stand_near(w, rock.x, rock.y, rock.plane), "no tile near the rock");
    rc_player_interact_object_at(w, (int)rock.obj_id, rock.x, rock.y, rock.plane, 0);
    CHECK(TICK_UNTIL(w, 200, count_item(p, COPPER_ORE) > 0), "no copper ore after 200 ticks");
    int empty = rc_world_object_active_id(w, (int)rock.obj_id, rock.x, rock.y, rock.plane);
    CHECK(empty == 11390 || empty == 11391, "rock did not empty (active %d)", empty);
}

static void fishing(RcWorld *w) {
    RcPlayer *p = w->player;
    int count = 0, uid = -1, sx = 0, sy = 0;
    const RcNpc *npcs = rc_get_npcs(w, &count);
    for (int i = 0; i < count && uid < 0; i++) {
        const RcNpcDef *def = rc_npc_base_def_for_npc(&npcs[i]);
        if (npcs[i].active && def && def->id == NET_SPOT) {
            uid = npcs[i].uid;
            sx = npcs[i].x;
            sy = npcs[i].y;
        }
    }
    CHECK(uid >= 0, "no net fishing spot");
    if (uid < 0) return;
    clear_inventory(w);
    give(w, SMALL_NET, 1);
    CHECK(stand_near(w, sx, sy, 0), "no shore tile near the spot");
    rc_player_interact_npc(w, uid, 0);
    CHECK(TICK_UNTIL(w, 300, count_item(p, RAW_SHRIMPS) > 0), "no shrimps after 300 ticks (at %d,%d; '%s', job %d)",
          p->x, p->y, p->interaction_outcome.message, p->skill_job.kind);
    CHECK(p->skills.xp[SKILL_FISHING] >= 10, "fishing xp %d", p->skills.xp[SKILL_FISHING]);
}

static void firemaking_and_cooking(RcWorld *w) {
    RcPlayer *p = w->player;
    clear_inventory(w);
    give(w, TINDERBOX, 1);
    give(w, LOGS, 1);
    give(w, RAW_SHRIMPS, 3);
    p->skills.base_level[SKILL_FIREMAKING] = p->skills.boosted_level[SKILL_FIREMAKING] = 50;
    p->skills.base_level[SKILL_COOKING] = p->skills.boosted_level[SKILL_COOKING] = 99;
    CHECK(rc_world_relocate_player(w, GYM_START_X, GYM_START_Y - 2, 0), "relocate to the road");
    int fx = p->x, fy = p->y;
    rc_player_use_inventory_item_on_inventory_item(w, 0, 1);
    CHECK(TICK_UNTIL(w, 100, count_item(p, LOGS) == 0), "logs not lit after 100 ticks ('%s', job %d)",
          p->interaction_outcome.message, p->skill_job.kind);
    rc_world_tick(w);
    rc_world_tick(w);
    RcObjectPlacement lit;
    CHECK(rc_world_object_current_placement(w, FIRE, fx, fy, 0, 0, &lit), "no fire at %d,%d", fx, fy);
    CHECK(p->x != fx || p->y != fy, "player did not step off the fire");
    CHECK(p->skills.xp[SKILL_FIREMAKING] >= 40, "firemaking xp %d", p->skills.xp[SKILL_FIREMAKING]);

    rc_player_interact_object_at(w, FIRE, fx, fy, 0, 0);
    CHECK(TICK_UNTIL(w, 40, count_item(p, SHRIMPS) == 3), "cooked %d of 3 shrimps", count_item(p, SHRIMPS));
    CHECK(p->skills.xp[SKILL_COOKING] >= 90, "cooking xp %d", p->skills.xp[SKILL_COOKING]);
}

static void smelting_and_smithing(RcWorld *w) {
    RcPlayer *p = w->player;
    RcObjectPlacement furnace, anvil;
    CHECK(find_object(w, FURNACE, FURNACE, &furnace), "no furnace in the gym");
    CHECK(find_object(w, 2097, 39620, &anvil), "no anvil in the gym");
    clear_inventory(w);
    give(w, COPPER_ORE, 2);
    give(w, TIN_ORE, 2);
    give(w, HAMMER, 1);
    int bar = make_option_for(w, BRONZE_BAR), dagger = make_option_for(w, BRONZE_DAGGER);
    CHECK(bar >= 0 && dagger >= 0, "make options: bar %d dagger %d of %d", bar, dagger, w->make_option_count);
    CHECK(stand_near(w, furnace.x, furnace.y, furnace.plane), "no tile near the furnace");
    rc_player_interact_object_at(w, FURNACE, furnace.x, furnace.y, furnace.plane, 1);
    CHECK(TICK_UNTIL(w, 60, p->make_menu == RC_MAKE_MENU_FURNACE), "furnace menu did not open ('%s')",
          p->interaction_outcome.message);
    CHECK(rc_skilling_make(w, bar, 0), "make bronze bars");
    CHECK(TICK_UNTIL(w, 40, count_item(p, BRONZE_BAR) == 2), "smelted %d of 2 bars", count_item(p, BRONZE_BAR));

    CHECK(stand_near(w, anvil.x, anvil.y, anvil.plane), "no tile near the anvil");
    rc_player_interact_object_at(w, (int)anvil.obj_id, anvil.x, anvil.y, anvil.plane, 0);
    CHECK(TICK_UNTIL(w, 60, p->make_menu == RC_MAKE_MENU_ANVIL), "anvil menu did not open");
    CHECK(rc_skilling_make(w, dagger, 1), "make a bronze dagger");
    CHECK(TICK_UNTIL(w, 40, count_item(p, BRONZE_DAGGER) == 1), "no dagger");
    CHECK(count_item(p, BRONZE_BAR) == 1, "dagger used %d bars", 2 - count_item(p, BRONZE_BAR));
    CHECK(p->skills.xp[SKILL_SMITHING] >= 6 + 6 + 12, "smithing xp %d", p->skills.xp[SKILL_SMITHING]);
}

int main(void) {
    struct stat st;
    if (stat("data/regions/mapsquare.catalog", &st) != 0) {
        fprintf(stderr, "gym data not built; run tools/build_gym.py\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config(1);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    CHECK(rc_world_install_content(w, rc_content_register_all), "content registration");
    CHECK(w->skilling && w->make_option_count > 60, "skilling rules: %d make options", w->make_option_count);
    CHECK(rc_world_relocate_player(w, GYM_START_X, GYM_START_Y, 0), "relocate to the start");  // loads the gym
    woodcutting_and_mining(w);
    fishing(w);
    firemaking_and_cooking(w);
    smelting_and_smithing(w);
    rc_world_destroy(w);
    if (failures) return 1;
    printf("gym skilling: PASS\n");
    return 0;
}
