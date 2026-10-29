// Gym bosses (run tools/build_gym.py first): the King Black Dragon's breath is cut by the
// anti-dragon shield and it drops dragon bones and black dragonhide; the Chaos Elemental teleports
// the player (Confusion) and takes their weapon off (Madness).
#include "../rc-core/api.h"
#include "../rc-core/config.h"
#include "../rc-core/items.h"
#include "../rc-core/npc.h"
#include "../rc-core/pathfinding.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    SKIP = 77, KBD = 239, CHAOS_ELEMENTAL = 2054, RUNE_SCIMITAR = 1333, ANTI_DRAGON_SHIELD = 1540,
    DRAGON_BONES = 536, BLACK_DRAGONHIDE = 1747, GOD_HP = 9900,
};

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const RcNpc *find_npc(const RcWorld *w, int id) {
    int count = 0;
    const RcNpc *npcs = rc_get_npcs(w, &count);
    for (int i = 0; i < count; i++) {
        const RcNpcDef *def = rc_npc_base_def_for_npc(&npcs[i]);
        if (npcs[i].active && !npcs[i].is_dead && def && def->id == id) return &npcs[i];
    }
    return NULL;
}

static int stand_near(RcWorld *w, int x, int y, int plane) {
    for (int r = 1; r <= 6; r++)
        for (int dx = -r; dx <= r; dx++)
            for (int dy = -r; dy <= r; dy++)
                if ((dx == -r || dx == r || dy == -r || dy == r)
                        && !rc_tile_blocked(&w->map, x + dx, y + dy, plane)
                        && rc_world_relocate_player(w, x + dx, y + dy, plane))
                    return 1;
    return 0;
}

static void arm(RcWorld *w, int shield) {
    RcPlayer *p = w->player;
    for (int s = 0; s < SKILL_COUNT; s++) p->skills.base_level[s] = p->skills.boosted_level[s] = 75;
    p->equipment[EQUIP_WEAPON] = (RcInvSlot){.item_id = RUNE_SCIMITAR, .quantity = 1};
    p->equipment[EQUIP_SHIELD] = (RcInvSlot){.item_id = shield, .quantity = shield > 0 ? 1 : 0};
    rc_recalc_bonuses(p);
    p->max_hp = p->current_hp = GOD_HP;
}

// Fights `boss` for `ticks` ticks with hitpoints topped up each tick; returns the biggest hit taken.
static int fight(RcWorld *w, int boss, int ticks, int *teleports, int *disarms) {
    RcPlayer *p = w->player;
    const RcNpc *npc = find_npc(w, boss);
    CHECK(npc != NULL, "boss %d not spawned", boss);
    if (!npc) return 0;
    CHECK(stand_near(w, npc->x, npc->y, npc->plane), "no tile near boss %d", boss);
    rc_player_attack_npc(w, npc->uid);
    int biggest = 0;
    for (int t = 0; t < ticks; t++) {
        int x = p->x, y = p->y, weapon = p->equipment[EQUIP_WEAPON].item_id;
        rc_world_tick(w);
        int hit = (GOD_HP - p->current_hp) / 10;
        if (hit > biggest) biggest = hit;
        p->current_hp = GOD_HP;
        if (teleports && (abs(p->x - x) > 2 || abs(p->y - y) > 2)) (*teleports)++;
        if (disarms && weapon > 0 && p->equipment[EQUIP_WEAPON].item_id < 0) {
            (*disarms)++;
            p->equipment[EQUIP_WEAPON] = (RcInvSlot){.item_id = RUNE_SCIMITAR, .quantity = 1};
            rc_recalc_bonuses(p);
        }
        if (p->attack_target < 0) {
            const RcNpc *b = find_npc(w, boss);
            if (b) rc_player_attack_npc(w, b->uid);
        }
    }
    return biggest;
}

static int ground_has(const RcWorld *w, int item, int x, int y) {
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (g->active && g->item_id == item && abs(g->x - x) <= 3 && abs(g->y - y) <= 3) return 1;
    }
    return 0;
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
    CHECK(rc_world_relocate_player(w, GYM_START_X, GYM_START_Y, 0), "relocate to the start");

    arm(w, -1);
    int unshielded = fight(w, KBD, 150, NULL, NULL);
    CHECK(unshielded > 30, "unprotected dragonfire peaked at %d", unshielded);
    arm(w, ANTI_DRAGON_SHIELD);
    int shielded = fight(w, KBD, 150, NULL, NULL);
    CHECK(shielded > 0 && shielded <= 26, "shielded KBD peaked at %d", shielded);

    // Finish the dragon off: its drop always has dragon bones and black dragonhide.
    const RcNpc *kbd = find_npc(w, KBD);
    if (kbd) {
        int uid = kbd->uid, kx = kbd->x, ky = kbd->y;
        rc_npc_resolve(w, uid)->current_hp = 1;
        for (int t = 0; t < 60; t++) {
            rc_world_tick(w);
            w->player->current_hp = GOD_HP;
            const RcNpc *b = rc_npc_resolve_const(w, uid);
            if (b) { kx = b->x; ky = b->y; }
            if (!b || b->is_dead) break;
            if (w->player->attack_target < 0) rc_player_attack_npc(w, uid);
        }
        for (int t = 0; t < 6; t++) rc_world_tick(w);
        CHECK(ground_has(w, DRAGON_BONES, kx, ky) && ground_has(w, BLACK_DRAGONHIDE, kx, ky),
              "KBD drop missing bones or hide");
    }

    arm(w, -1);
    int teleports = 0, disarms = 0;
    fight(w, CHAOS_ELEMENTAL, 300, &teleports, &disarms);
    CHECK(teleports > 0, "Chaos Elemental never teleported the player");
    CHECK(disarms > 0, "Chaos Elemental never unequipped the weapon");

    rc_world_destroy(w);
    if (failures) return 1;
    printf("gym bosses: PASS\n");
    return 0;
}
