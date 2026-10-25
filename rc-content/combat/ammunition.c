#include "ammunition.h"

#include "items.h"
#include "combat.h"
#include "prayer.h"
#include "npc.h"
#include "config.h"
#include "pathfinding.h"
#include "rng.h"
#include <string.h>

enum { ARROW, BOLT, JAVELIN, OGRE, TRAINING, KEBBIT, RACK, ANTLER, ATLATL, TAR, BONE };

// Reviewed ammunition families, including poison and enchanted variants.
static const struct { int kind, tier; int ids[32]; } ammunition[] = {
    {ARROW, 1, {882,883,5616,5622,884,885,5617,5623}},
    {ARROW, 5, {886,887,5618,5624}},
    {ARROW, 20, {888,889,5619,5625}},
    {ARROW, 30, {890,891,5620,5626}},
    {ARROW, 40, {892,893,5621,5627,78}},
    {ARROW, 50, {21326,21332,21334,21336,4160}},
    {ARROW, 60, {11212,11227,11228,11229}},
    {BOLT, 1, {877,878,6061,6062,879,9236}},
    {BOLT, 16, {9139,9286,9293,9300,9335,9237}},
    {BOLT, 26, {9140,9287,9294,9301,880,9238,9145,9292,9299,9306}},
    {BOLT, 31, {9141,9288,9295,9302,9336,9239}},
    {BOLT, 36, {9142,9289,9296,9303,9337,9240,9338,9241}},
    {BOLT, 46, {9143,9290,9297,9304,9339,9242,9340,9243}},
    {BOLT, 61, {9144,9291,9298,9305,11875,21316,9341,9244,9342,9245}},
    {BOLT, 64, {21905,21924,21926,21928,21955,21932,21957,21934,21959,
        21936,21961,21938,21963,21940,21965,21942,21967,21944,21969,
        21946,21971,21948,21973,21950}},
    {JAVELIN, 1, {825,831,5642,5648,826,832,5643,5649,827,833,5644,5650,
        828,834,5645,5651,829,835,5646,5652,830,836,5647,5653,
        21318,21320,21322,21324,19484,19486,19488,19490}},
    {OGRE, 1, {2866,4773,4778,4783,4788,4793}},
    {OGRE, 2, {4798,4803}},
    {TRAINING, 1, {9706}},
    {KEBBIT, 1, {10158,10159}},
    {RACK, 1, {4740}},
    {ANTLER, 1, {28872,28878}},
    {ATLATL, 1, {28991}},
    {BONE, 1, {8882}},
    {TAR, 1, {10142}}, {TAR, 2, {10143}}, {TAR, 3, {10144}},
    {TAR, 4, {10145}}, {TAR, 5, {28837}},
};

// Names group cosmetic/degradation variants whose installed definitions agree.
static const struct { const char *name; int kind, tier; } weapons[] = {
    {"Shortbow", ARROW, 1}, {"Longbow", ARROW, 1},
    {"Cursed goblin bow", ARROW, 1}, {"Rain bow", ARROW, 1},
    {"Oak shortbow", ARROW, 5}, {"Oak longbow", ARROW, 5},
    {"Signed oak bow", ARROW, 5},
    {"Willow shortbow", ARROW, 20}, {"Willow longbow", ARROW, 20},
    {"Willow comp bow", ARROW, 20},
    {"Maple shortbow", ARROW, 30}, {"Maple longbow", ARROW, 30},
    {"Yew shortbow", ARROW, 40}, {"Yew longbow", ARROW, 40},
    {"Yew comp bow", ARROW, 40},
    {"Magic shortbow", ARROW, 50}, {"Magic shortbow (i)", ARROW, 50},
    {"Magic longbow", ARROW, 50}, {"Magic comp bow", ARROW, 50},
    {"Bone shortbow", ARROW, 50}, {"Seercull", ARROW, 50},
    {"Dark bow", ARROW, 60}, {"Dark bow (bh)", ARROW, 60},
    {"3rd age bow", ARROW, 60}, {"Twisted bow", ARROW, 60},
    {"Scorching bow", ARROW, 60},
    {"Venator bow (uncharged)", ARROW, 60},
    {"Crossbow", BOLT, 1}, {"Phoenix crossbow", BOLT, 1},
    {"Bronze crossbow", BOLT, 1}, {"Blurite crossbow", BOLT, 16},
    {"Iron crossbow", BOLT, 26}, {"Steel crossbow", BOLT, 31},
    {"Mithril crossbow", BOLT, 36}, {"Adamant crossbow", BOLT, 46},
    {"Rune crossbow", BOLT, 61}, {"Dragon crossbow", BOLT, 64},
    {"Armadyl crossbow", BOLT, 64}, {"Dragon hunter crossbow", BOLT, 64},
    {"Zaryte crossbow", BOLT, 64}, {"Dorgeshuun crossbow", BOLT, 26},
    {"Light ballista", JAVELIN, 1}, {"Heavy ballista", JAVELIN, 1},
    {"Ogre bow", OGRE, 1}, {"Comp ogre bow", OGRE, 2},
    {"Training bow", TRAINING, 1}, {"Hunters' crossbow", KEBBIT, 1},
    {"Hunters' sunlight crossbow", ANTLER, 1},
    {"Karil's crossbow", RACK, 1}, {"Karil's crossbow 100", RACK, 1},
    {"Karil's crossbow 75", RACK, 1}, {"Karil's crossbow 50", RACK, 1},
    {"Karil's crossbow 25", RACK, 1}, {"Eclipse atlatl", ATLATL, 1},
    {"Swamp lizard", TAR, 1}, {"Orange salamander", TAR, 2},
    {"Red salamander", TAR, 3}, {"Black salamander", TAR, 4},
    {"Tecu salamander", TAR, 5},
};

static bool recoverable_ammunition(const RcItemDef *def) {
    if (!def) return false;
    if (def->equip_slot == EQUIP_WEAPON && def->weapon_type == 23
            && (strstr(def->name, "dart") || strstr(def->name, "knife"))) return true;
    for (unsigned i = 0; i < sizeof(ammunition) / sizeof(ammunition[0]); i++) {
        if (ammunition[i].kind != ARROW && ammunition[i].kind != BOLT) continue;
        for (int j = 0; j < 32 && ammunition[i].ids[j]; j++)
            if (ammunition[i].ids[j] == def->id) return true;
    }
    return false;
}

int rc_content_consume_ammunition(RcWorld *world, int slot, int count,
    int x, int y, int plane, int delay, const char **failure) {
    *failure = NULL;
    if (!world || slot < 0 || slot >= RC_EQUIP_COUNT || count <= 0 || count > 4) {
        *failure = "Invalid ammunition payment.";
        return 0;
    }
    RcInvSlot ammo = world->player.equipment[slot];
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, world).code != RC_ITEM_RESULT_OK
            || rc_item_tx_remove_equipment(&tx, slot, count, ammo.generation).code
                != RC_ITEM_RESULT_OK) {
        *failure = "Ammunition changed before the attack could launch.";
        return 0;
    }
    uint32_t rng = world->rng_state;
    int recovered = 0;
    if ((world->enabled & RC_SUB_LOOT)
            && recoverable_ammunition(rc_item_def_get(ammo.item_id))
            && !rc_tile_blocked(&world->map, x, y, plane))
        for (int i = 0; i < count; i++)
            if (rc_rng_range(&world->rng_state, 4) != 0) recovered++;
    if (!recovered) {
        if (rc_item_tx_commit(&tx).code == RC_ITEM_RESULT_OK) return 1;
    } else {
        RcGroundGrant grant = {ammo.item_id, recovered, ammo.state_id};
        RcGroundPolicy policy = {100, 200, delay, 300};
        RcGroundGrantResult result = rc_item_tx_commit_ground(&tx, &grant, 1,
            x, y, plane, RC_GROUND_OWNER_LOCAL_PLAYER, policy);
        if (result == RC_GROUND_GRANT_OK) return 1;
    }
    world->rng_state = rng;
    *failure = "Cannot reserve recovered ammunition; attack and payment cancelled.";
    return 0;
}

int rc_content_ranged_resource_slot(const RcPlayer *player, const char **failure) {
    const RcItemDef *weapon = player
        ? rc_item_def_get(player->equipment[EQUIP_WEAPON].item_id) : NULL;
    *failure = "Ranged weapon or ammunition rules are unavailable.";
    if (!weapon) return RC_RANGED_RESOURCE_INVALID;
    if (weapon->id == 28919 || weapon->id == 28922)
        return RC_RANGED_RESOURCE_NONE;
    if (weapon->stackable && weapon->equip_slot == EQUIP_WEAPON
            && (weapon->weapon_type == 7 || weapon->weapon_type == 23))
        return EQUIP_WEAPON;
    if (strcmp(weapon->name, "Bow of faerdhinen (c)") == 0)
        return RC_RANGED_RESOURCE_NONE;
    if (strstr(weapon->name, "rystal bow") || strstr(weapon->name, "faerdhinen")
            || strstr(weapon->name, "blowpipe") || strstr(weapon->name, "Craw's bow")
            || strstr(weapon->name, "Webweaver bow")) {
        *failure = "This weapon requires internal charges/ammunition; that container is not implemented yet.";
        return RC_RANGED_RESOURCE_INVALID;
    }
    if (strcmp(weapon->name, "Venator bow") == 0) {
        *failure = "Charged Venator bow effects are not implemented; use its uncharged form.";
        return RC_RANGED_RESOURCE_INVALID;
    }
    int kind = -1, tier = 0;
    for (unsigned i = 0; i < sizeof(weapons) / sizeof(weapons[0]); i++) {
        if (strcmp(weapons[i].name, weapon->name) == 0) {
            kind = weapons[i].kind;
            tier = weapons[i].tier;
            break;
        }
    }
    if (kind < 0) return RC_RANGED_RESOURCE_INVALID;
    const RcInvSlot *ammo = &player->equipment[EQUIP_AMMO];
    if (ammo->item_id < 0 || ammo->quantity <= 0) {
        *failure = "There is no ammunition in your ammo slot.";
        return RC_RANGED_RESOURCE_INVALID;
    }
    if (strcmp(weapon->name, "Dorgeshuun crossbow") == 0) {
        static const int bone_crossbow_ammo[] = {
            8882, 877, 878, 6061, 6062, 9139, 9286, 9293, 9300,
            9140, 9287, 9294, 9301,
        };
        for (unsigned i = 0; i < sizeof(bone_crossbow_ammo) / sizeof(bone_crossbow_ammo[0]); i++)
            if (ammo->item_id == bone_crossbow_ammo[i]) return EQUIP_AMMO;
        *failure = "The bone crossbow cannot fire silver or gem-tipped bolts, or metal bolts above iron.";
        return RC_RANGED_RESOURCE_INVALID;
    }
    for (unsigned i = 0; i < sizeof(ammunition) / sizeof(ammunition[0]); i++) {
        for (unsigned j = 0; j < 32 && ammunition[i].ids[j]; j++) {
            if (ammunition[i].ids[j] != ammo->item_id) continue;
            if (ammunition[i].kind == kind && ammunition[i].tier <= tier
                    && (kind != TAR || ammunition[i].tier == tier))
                return EQUIP_AMMO;
            *failure = "That ammunition is incompatible with this weapon or exceeds its ammunition tier.";
            return RC_RANGED_RESOURCE_INVALID;
        }
    }
    *failure = "That item is not supported ammunition for this weapon.";
    return RC_RANGED_RESOURCE_INVALID;
}

void rc_content_ranged_calc(const RcWorld *world, const RcNpc *target, RcCombatCalc *calc) {
    const RcPlayer *p = &world->player;
    const RcItemDef *weapon = rc_item_def_get(p->equipment[EQUIP_WEAPON].item_id);
    if (!weapon) return;
    if (!strcmp(weapon->name, "Eclipse atlatl")) {
        int level = p->skills.boosted_level[SKILL_STRENGTH];
        int prayer = rc_prayer_ranged_strength_bonus(p->active_prayers);
        int effective = prayer == 5 && level <= 20 ? level + 1 : level * (100 + prayer) / 100;
        effective += 8 + (p->attack_stance == RC_ATTACK_STANCE_ACCURATE ? 3 : 0);
        calc->max_hit = (effective * (p->equipment_bonuses[EQ_STR] + 64) + 320) / 640;
        if (calc->max_hit < 0) calc->max_hit = 0;
    }
    if (weapon->weapon_type == 7) {
        const RcNpcDef *def = rc_npc_def_for_npc(world, target);
        int size = def && def->size > 0 ? def->size : 1;
        int dx = p->x - (target->x + size / 2), dy = p->y - (target->y + size / 2);
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        int distance = dx > dy ? dx : dy;
        int factor = p->attack_style_idx == 0 ? (distance < 4 ? 4 : distance < 7 ? 3 : 2)
            : p->attack_style_idx == 1 ? (distance < 4 || distance >= 7 ? 3 : 4)
            : (distance < 4 ? 2 : distance < 7 ? 3 : 4);
        calc->attack_roll = calc->attack_roll * factor / 4;
        calc->hit_chance = rc_hit_chance(calc->attack_roll, calc->defence_roll);
    }
}
