#include "layout.h"

#include "assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *rc_gym_mode_name(const char *mode) {
    if (mode && mode[0]) return mode;
    const char *env = getenv("RS_GYM_MODE");
    return env && env[0] ? env : RC_GYM_DEFAULT_MODE;
}

static void mode_path(char *out, const RcGymLayout *layout, const char *rel) {
    snprintf(out, RC_GYM_PATH_LEN, "%.200s/%.100s", layout->dir, rel);
}

int rc_gym_layout_load_mode(const char *mode, RcGymLayout *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    mode = rc_gym_mode_name(mode);
    snprintf(out->mode, sizeof(out->mode), "%s", mode);
    snprintf(out->dir, sizeof(out->dir), "data/modes/%.63s", mode);
    char path[RC_GYM_PATH_LEN];
    mode_path(path, out, "gym.cfg");
    FILE *f = rc_asset_fopen(path, "r");
    if (!f) {
        fprintf(stderr, "gym layout: cannot open %s (run tools/build_mode.py %s)\n", path, mode);
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
    mode_path(out->placements, out, "regions/world.object-placements.indexed.bin");
    mode_path(out->collision, out, "regions/world.collision-tiles.indexed.bin");
    mode_path(out->npc_spawns, out, "spawns/world.npc-spawns.indexed.bin");
    mode_path(out->ground_items, out, "spawns/world.ground-items.indexed.bin");
    mode_path(out->area_flags, out, "defs/area_flags.bin");
    mode_path(out->traversal, out, "defs/traversal_edges.bin");
    mode_path(out->nodes, out, "defs/gathering_nodes.bin");
    mode_path(out->drops, out, "defs/drops.bin");
    mode_path(out->rules, out, "mode.rules");
    mode_path(out->regions, out, "regions");
    mode_path(out->spawn_objects, out, "models/spawn_objects.models");
    out->has_rules = rc_asset_exists(out->rules);
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
    cfg.drops_path = layout ? layout->drops : "data/defs/drops.bin";
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
    cfg.object_placements_path = layout ? layout->placements : NULL;
    cfg.object_behaviors_path = "data/defs/object_behaviors.bin";
    cfg.collision_tiles_path = layout ? layout->collision : NULL;
    cfg.spawns_path = layout ? layout->npc_spawns : NULL;
    cfg.ground_item_spawns_path = layout ? layout->ground_items : NULL;
    cfg.area_flags_path = layout ? layout->area_flags : NULL;
    cfg.traversal_edges_path = layout ? layout->traversal : NULL;
    cfg.recipes_path = "data/defs/recipes.bin";
    cfg.skill_drops_path = "data/defs/skill_drops.bin";
    cfg.gathering_nodes_path = layout ? layout->nodes : NULL;
    cfg.mode_rules_path = layout && layout->has_rules ? layout->rules : NULL;
    cfg.seed = 12345;
    cfg.player_count = players;
    if (layout) rc_gym_layout_apply(layout, &cfg);
    return cfg;
}
