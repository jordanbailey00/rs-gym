// Headless checks on the built gym (run tools/build_gym.py first): the world loads,
// the player starts on the island, the sea blocks movement, NPCs spawn and the bank opens.
#include "../rc-core/api.h"
#include "../rc-core/config.h"
#include "../rc-core/pathfinding.h"
#include "../rc-content/content.h"
#include "../rc-viewer/dev_validation.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { START_X = RS_GYM_START_X, START_Y = RS_GYM_START_Y, BANK_BOOTH = 10355, SKIP = 77 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// Same subsystems and data paths as rc-viewer's world.
static RcWorldConfig gym_config(void) {
    RcWorldConfig cfg = rc_preset_base_only();
    cfg.npc_capacity = RC_WORLD_NPC_CAPACITY_FULL;
    cfg.subsystems = RC_SUB_INVENTORY | RC_SUB_EQUIPMENT | RC_SUB_LOOT | RC_SUB_COMBAT | RC_SUB_PRAYER |
                     RC_SUB_OBJECTS | RC_SUB_REGIONS | RC_SUB_TRAVERSAL | RC_SUB_STORAGE |
                     RC_SUB_ENCOUNTER | RC_SUB_CONSUMABLES;
    cfg.npc_defs_path = "data/defs/npc_defs.bin";
    cfg.items_path = "data/defs/items.bin";
    cfg.drops_path = "data/defs/drops.bin";
    cfg.rdt_path = "data/defs/rdt.bin";
    cfg.gdt_path = "data/defs/gdt.bin";
    cfg.mrdt_path = "data/defs/mrdt.bin";
    cfg.prayers_path = "data/defs/prayers.bin";
    cfg.varbits_path = "data/defs/varbits.bin";
    cfg.varps_path = "data/defs/varps.bin";
    cfg.spells_path = "data/defs/spells.bin";
    cfg.combat_profiles_path = "data/defs/combat_visuals.tsv";
    cfg.monster_mechanics_path = "data/defs/regular_npc_mechanics.bin";
    cfg.activity_schemas_path = "data/defs/activity_schemas.bin";
    cfg.activity_spawns_path = "data/defs/activity_spawns.bin";
    cfg.activity_mechanics_path = "data/defs/activity_mechanics.bin";
    cfg.activity_states_path = "data/defs/activity_states.bin";
    cfg.encounters_path = "data/defs/encounters.bin";
    cfg.player_actions_path = "data/defs/player_actions.bin";
    cfg.object_defs_path = "data/defs/object_defs.bin";
    cfg.object_placements_path = "data/regions/world.object-placements.indexed.bin";
    cfg.object_behaviors_path = "data/defs/object_behaviors.bin";
    cfg.collision_tiles_path = "data/regions/world.collision-tiles.indexed.bin";
    cfg.spawns_path = "data/spawns/world.npc-spawns.indexed.bin";
    cfg.ground_item_spawns_path = "data/spawns/world.ground-items.indexed.bin";
    cfg.area_flags_path = "data/defs/area_flags.bin";
    cfg.traversal_edges_path = "data/defs/traversal_edges.bin";
    cfg.seed = 12345;
    return cfg;
}

static void tick(RcWorld *w, int n) {
    for (int i = 0; i < n; i++) rc_world_tick(w);
}

static int bank_option(const RcWorld *w, int x, int y) {
    for (int op = 0; op < RC_OBJECT_ACTIONS; op++) {
        if (rc_world_object_option_supported(w, BANK_BOOTH, x, y, 0, 0, op)
                && strcmp(rc_world_object_option_label(w, BANK_BOOTH, x, y, 0, 0, op), "Bank") == 0)
            return op;
    }
    return -1;
}

int main(void) {
    struct stat st;
    if (stat("data/regions/mapsquare.catalog", &st) != 0) {
        fprintf(stderr, "gym data not built; run tools/build_gym.py\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_config();
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    CHECK(rc_world_install_content(w, rc_content_register_all), "content registration");
    CHECK(rc_world_relocate_player(w, START_X, START_Y, 0), "relocate to the gym start");
    const RcPlayer *p = rc_get_player(w);
    CHECK(p->x == START_X && p->y == START_Y, "player at %d,%d", p->x, p->y);

    int npc_count = 0, active = 0;
    const RcNpc *npcs = rc_get_npcs(w, &npc_count);
    for (int i = 0; i < npc_count; i++) active += npcs[i].active;
    CHECK(active > 0, "no NPCs spawned");

    // The ocean ring (world x 64..127) is blocked. Clicking just past the west coast walks
    // to the shore and stops on land, like an OSRS click on water.
    CHECK(rc_tile_blocked(&w->map, 100, START_Y, 0), "ocean ring tile is walkable");
    int coast_x = 128;
    while (coast_x < START_X && rc_tile_blocked(&w->map, coast_x, START_Y, 0)) coast_x++;
    CHECK(coast_x > 128 && coast_x < START_X, "no west coast found on row %d", START_Y);
    CHECK(rc_player_walk_to(w, coast_x - 3, START_Y), "walk command rejected");
    tick(w, 120);
    CHECK(p->x >= coast_x && p->x <= coast_x + 2 && !rc_tile_blocked(&w->map, p->x, p->y, 0),
          "west walk ended at %d,%d (coast %d)", p->x, p->y, coast_x);

    // Draynor bank booths, stamped at area (86, 98): source booth (3091, 3242) lands on (219, 232).
    CHECK(rc_world_relocate_player(w, START_X, START_Y, 0), "relocate back to start");
    int op = bank_option(w, 219, 232);
    CHECK(op >= 0, "bank booth at 219,232 offers no Bank option");
    if (op >= 0) {
        rc_player_interact_object_at(w, BANK_BOOTH, 219, 232, 0, op);
        for (int i = 0; i < 120 && !p->storage_kind; i++) tick(w, 1);
        CHECK(p->storage_kind != 0, "bank did not open; player ended at %d,%d", p->x, p->y);
    }

    rc_world_destroy(w);
    if (failures) return 1;
    printf("gym world: PASS (npcs=%d)\n", active);
    return 0;
}
