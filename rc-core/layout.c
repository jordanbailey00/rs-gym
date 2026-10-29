#include "layout.h"

#include "assets.h"

#include <stdio.h>
#include <string.h>

int rc_gym_layout_load(const char *path, RcGymLayout *out) {
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));
    FILE *f = rc_asset_fopen(path, "r");
    if (!f) {
        fprintf(stderr, "gym layout: cannot open %s (run tools/build_gym.py)\n", path);
        return 0;
    }
    char line[256];
    int seen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "name %63s", out->name) == 1) seen |= 1;
        else if (sscanf(line, "start %d %d %d", &out->start_x, &out->start_y, &out->start_plane) == 3) seen |= 2;
        else if (sscanf(line, "respawn %d %d %d", &out->respawn_x, &out->respawn_y, &out->respawn_plane) == 3) seen |= 4;
        else if (sscanf(line, "area %d %d %d %d", &out->min_rx, &out->min_ry, &out->max_rx, &out->max_ry) == 4) seen |= 8;
        else if (out->landmark_count < RC_GYM_MAX_LANDMARKS) {
            RcGymLandmark *lm = &out->landmarks[out->landmark_count];
            if (sscanf(line, "landmark %23s %d %d %d", lm->name, &lm->x, &lm->y, &lm->plane) == 4)
                out->landmark_count++;
        }
    }
    rc_asset_close(f);
    if (seen != 15) {
        fprintf(stderr, "gym layout: %s is missing name/start/respawn/area\n", path);
        return 0;
    }
    return 1;
}

void rc_gym_layout_apply(const RcGymLayout *layout, RcWorldConfig *cfg) {
    if (!layout || !cfg) return;
    cfg->pin_area = true;
    cfg->pinned_min_rx = layout->min_rx;
    cfg->pinned_min_ry = layout->min_ry;
    cfg->pinned_max_rx = layout->max_rx;
    cfg->pinned_max_ry = layout->max_ry;
    cfg->respawn_x = layout->respawn_x;
    cfg->respawn_y = layout->respawn_y;
    cfg->respawn_plane = layout->respawn_plane;
}

RcWorldConfig rc_gym_world_config(const RcGymLayout *layout, int players) {
    RcWorldConfig cfg = rc_preset_base_only();
    cfg.npc_capacity = RC_WORLD_NPC_CAPACITY_FULL;
    cfg.subsystems = RC_SUB_INVENTORY | RC_SUB_EQUIPMENT | RC_SUB_LOOT
                   | RC_SUB_COMBAT | RC_SUB_PRAYER | RC_SUB_OBJECTS
                   | RC_SUB_REGIONS | RC_SUB_TRAVERSAL | RC_SUB_STORAGE
                   | RC_SUB_ENCOUNTER | RC_SUB_CONSUMABLES | RC_SUB_SKILLS;
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
    cfg.recipes_path = "data/defs/recipes.bin";
    cfg.skill_drops_path = "data/defs/skill_drops.bin";
    cfg.gathering_nodes_path = "data/defs/gathering_nodes.bin";
    cfg.seed = 12345;
    cfg.player_count = players;
    if (layout) rc_gym_layout_apply(layout, &cfg);
    return cfg;
}
