#include "../../rc-core/players.h"
#include "magic.h"
#include "items.h"
#include "combat_formula.h"
#include "npc.h"
#include "prayer.h"
#include "ralos.h"
#include "ammunition.h"
#include "mode.h"
#include "rng.h"
#include <string.h>

typedef struct {
    int charged, empty, capacity, divisor, offset, speed;
} PoweredStaff;

// B237 item identities; max-hit formulas checked against modern OSRS references.
static const PoweredStaff powered[] = {
    {11905, 11908, 2500, 3, -5, 4},
    {11907, 11908, 2500, 3, -5, 4},
    {12899, 12900, 2500, 3, -2, 4},
    {22288, 22290, 20000, 3, -5, 4},
    {22292, 22294, 20000, 3, -2, 4},
    {22323, 22481, 20000, 3, 0, 4},
    {25731, 25733, 20000, 3, 0, 4},
    {27275, 27277, 20000, 3, 1, 5},
    {28547, 28549, 20000, 3, 1, 5},
    {22555, 22552, 17000, 3, -8, 4},
    {27665, 27662, 17000, 3, -6, 4},
    {28585, 28583, 20000, 37, 96, 5},
    {31113, 31115, 50000, 3, -6, 3},
    {22516, -1, 0, 3, -2, 4},
    {33314, 33316, 2500, 3, -2, 4},
    {33318, 33320, 20000, 3, -2, 4},
    {33322, 33322, 2500, 3, -5, 4},
    {33323, 33322, 2500, 3, -5, 4},
    {33326, 33328, 20000, 3, -5, 4},
};

static const PoweredStaff *powered_staff(int id) {
    for (unsigned i = 0; i < sizeof(powered) / sizeof(powered[0]); i++)
        if (powered[i].charged == id) return &powered[i];
    return NULL;
}

static unsigned spell_target_attributes(int id) {
    static const struct { int id; unsigned attributes; } targets[] = {
#include "npc_spell_targets.inc"
    };
    int low = 0, high = sizeof(targets) / sizeof(targets[0]);
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (targets[mid].id < id) low = mid + 1;
        else high = mid;
    }
    return low < (int)(sizeof(targets) / sizeof(targets[0])) && targets[low].id == id
        ? targets[low].attributes : 0;
}

uint32_t rc_content_magic_charge_capacity(int id) {
    const PoweredStaff *staff = powered_staff(id);
    if (staff) return staff->capacity;
    if (id == 1409 || id == 33330) return 120;
    if (id == 12658 || id == 33332) return 2500;
    if (id == 27679 || id == 27788) return 17000;
    return 0;
}

int rc_content_magic_consume_charge(RcWorld *world, int id) {
    if (world->player->combat_style != COMBAT_MAGIC)
        return rc_content_ralos_consume_charge(world, id);
    uint32_t capacity = rc_content_magic_charge_capacity(id);
    if (!capacity) return rc_content_ralos_consume_charge(world, id);
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, world).code != RC_ITEM_RESULT_OK) return 0;
    RcInvSlot *weapon = &tx.equipment[EQUIP_WEAPON];
    if (weapon->item_id != id || !weapon->state_id || weapon->state_id > capacity)
        return 0;
    const PoweredStaff *staff = powered_staff(id);
    if (--weapon->state_id == 0 && staff) weapon->item_id = staff->empty;
    return rc_item_tx_commit(&tx).code == RC_ITEM_RESULT_OK;
}

int rc_content_can_autocast(const RcPlayer *p, const RcSpellDef *spell) {
    const RcItemDef *weapon = rc_item_def_get(p->equipment[EQUIP_WEAPON].item_id);
    if (!weapon || !spell || spell->max_hit <= 0 || powered_staff(weapon->id)) return 0;
    const char *name = weapon->name;
    if (spell->book == RC_SPELL_BOOK_ANCIENT) {
        if (strstr(name, "Harmonised")) return 0;
        return strstr(name, "Ancient") || strstr(name, "ancient sceptre") ||
            strstr(name, "Kodai") || strstr(name, "Master wand") ||
            strstr(name, "Nightmare") || strstr(name, "nightmare") ||
            strstr(name, "Blue moon") ||
            (strstr(name, "Ahrim") && p->equipment[EQUIP_AMULET].item_id == 12851);
    }
    if (spell->book == RC_SPELL_BOOK_ARCEUUS)
        return strstr(name, "Slayer") || strstr(name, "Purging") ||
            strstr(name, "staff of the dead") || strstr(name, "Staff of the dead") ||
            strstr(name, "Staff of light") || strstr(name, "Staff of balance") ||
            strstr(name, "Blue moon");
    if (spell->book != RC_SPELL_BOOK_STANDARD) return 0;
    if (!strcmp(spell->name, "Iban Blast")) return strstr(name, "Iban") != NULL;
    if (!strcmp(spell->name, "Magic Dart"))
        return strstr(name, "Slayer") || strstr(name, "Staff of the dead") ||
            strstr(name, "staff of the dead") || strstr(name, "Staff of light") ||
            strstr(name, "Staff of balance");
    if (!strcmp(spell->name, "Saradomin Strike")) return strstr(name, "Staff of light") != NULL;
    if (!strcmp(spell->name, "Claws of Guthix"))
        return strstr(name, "Staff of balance") || strstr(name, "Void knight mace");
    if (!strcmp(spell->name, "Flames of Zamorak"))
        return strstr(name, "staff of the dead") || strstr(name, "Staff of the dead") ||
            strstr(name, "sceptre (a)");
    if (!strcmp(spell->name, "Crumble Undead"))
        return strstr(name, "Slayer") || strstr(name, "staff of the dead") ||
            strstr(name, "Staff of the dead") || strstr(name, "Staff of light") ||
            strstr(name, "Staff of balance") || strstr(name, "Void knight mace") ||
            strstr(name, "Skull sceptre (i)");
    return !strncmp(spell->name, "Wind ", 5) || !strncmp(spell->name, "Water ", 6) ||
        !strncmp(spell->name, "Earth ", 6) || !strncmp(spell->name, "Fire ", 5);
}

int rc_content_prepare_magic(RcWorld *world, const RcNpc *target,
                              const RcSpellDef *spell, RcCombatCalc *calc,
                              int *speed, const char **failure) {
    const RcPlayer *p = world->player;
    const RcInvSlot *held = &p->equipment[EQUIP_WEAPON];
    const RcItemDef *weapon = rc_item_def_get(held->item_id);
    int level = p->skills.boosted_level[SKILL_MAGIC];
    int max_hit;
    if (spell) {
        const RcNpcDef *npc = rc_npc_def_for_npc(world, target);
        unsigned required = !strcmp(spell->name, "Crumble Undead") ? 1
            : strstr(spell->name, "Demonbane") ? 2 : 0;
        if (required && (!npc || !(spell_target_attributes(npc->id) & required))) {
            *failure = required == 1 ? "This target is not a reviewed undead spell target."
                                     : "This target is not a reviewed demon spell target.";
            return 0;
        }
        if (level < spell->level) {
            *failure = "Your Magic level is too low for this spell.";
            return 0;
        }
        if (p->manual_spell_cast < 0 && !rc_content_can_autocast(p, spell)) {
            *failure = "This weapon cannot autocast that spell.";
            return 0;
        }
        int god_staff = !strcmp(spell->name, "Saradomin Strike") ? 2415 :
            !strcmp(spell->name, "Claws of Guthix") ? 2416 :
            !strcmp(spell->name, "Flames of Zamorak") ? 2417 : -1;
        if (god_staff >= 0 && held->item_id != god_staff && !rc_content_can_autocast(p, spell)) {
            *failure = "This god spell requires its corresponding staff.";
            return 0;
        }
        max_hit = spell->max_hit;
        // Elemental spells scale to the strongest element of their tier the caster has unlocked,
        // read from the spell table (a mode's whitelist and levels included).
        if (rc_spell_element(spell->name)) {
            const char *tier = strchr(spell->name, ' ') + 1;
            for (int i = 0; i < RC_MAX_SPELL_DEFS; i++) {
                const RcSpellDef *s = rc_spell_def_get(i);
                if (!s || !s->loaded || s->book != spell->book || level < s->level
                        || !rc_spell_element(s->name) || strcmp(strchr(s->name, ' ') + 1, tier))
                    continue;
                if (s->max_hit > max_hit) max_hit = s->max_hit;
            }
        }
        if (!strcmp(spell->name, "Magic Dart")) {
            if (!weapon || (!strstr(weapon->name, "Slayer") &&
                    !strstr(weapon->name, "staff of the dead") &&
                    !strstr(weapon->name, "Staff of the dead") &&
                    !strstr(weapon->name, "Staff of light") &&
                    !strstr(weapon->name, "Staff of balance"))) {
                *failure = "Magic Dart requires a compatible Slayer staff.";
                return 0;
            }
            max_hit = 10 + level / 10;
        }
        if (!strcmp(spell->name, "Iban Blast") &&
                (!weapon || !strstr(weapon->name, "Iban") || !held->state_id ||
                 held->state_id > rc_content_magic_charge_capacity(held->item_id))) {
            *failure = "Iban Blast requires an Iban's staff with charges.";
            return 0;
        }
        if (weapon && weapon->id == 24423 && spell->book == RC_SPELL_BOOK_STANDARD
                && p->manual_spell_cast < 0) *speed = 4;
    } else {
        if (weapon && weapon->weapon_type == 26) {
            static const struct { const char *name; int strength; } salamanders[] = {
                {"Swamp lizard", 56}, {"Orange salamander", 59}, {"Red salamander", 77},
                {"Black salamander", 92}, {"Tecu salamander", 104},
            };
            for (unsigned i = 0; i < sizeof(salamanders) / sizeof(salamanders[0]); i++) {
                if (strcmp(weapon->name, salamanders[i].name)) continue;
                if (rc_content_ranged_resource_slot(p, failure) != EQUIP_AMMO) return 0;
                *calc = rc_calc_magic(p, target, (level * (salamanders[i].strength + 64) + 320) / 640);
                return 1;
            }
        }
        const PoweredStaff *staff = powered_staff(held->item_id);
        if (!staff || (staff->capacity &&
                (!held->state_id || held->state_id > (uint32_t)staff->capacity))) {
            *failure = "This powered weapon has no valid charges. Use a charged version.";
            return 0;
        }
        max_hit = staff->divisor == 37 ? (8 * level + staff->offset) / 37
                                     : level / staff->divisor + staff->offset;
        if (max_hit < 1) max_hit = 1;
        *speed = staff->speed;
    }
    *calc = rc_calc_magic(p, target, max_hit);
    // Elemental weakness: a matching spell gains the weakness percent in accuracy and that share of
    // its base max hit in damage.
    const RcNpcDef *target_def = spell ? rc_npc_def_for_npc(world, target) : NULL;
    if (target_def && target_def->weakness_pct
            && target_def->weakness_element == rc_spell_element(spell->name)) {
        calc->attack_roll += calc->attack_roll * target_def->weakness_pct / 100;
        calc->hit_chance = rc_hit_chance(calc->attack_roll, calc->defence_roll);
        calc->max_hit += max_hit * target_def->weakness_pct / 100;
    }
    const RcInvSlot *shield = &p->equipment[EQUIP_SHIELD];
    if (spell && shield->quantity > 0 && shield->state_id > 0 &&
        ((shield->item_id == 20714 && !strncmp(spell->name, "Fire ", 5)) ||
         (shield->item_id == 25574 && !strncmp(spell->name, "Water ", 6)) ||
         (shield->item_id == 30064 && !strncmp(spell->name, "Earth ", 6))))
        calc->max_hit = calc->max_hit * 11 / 10;
    if (!spell && (held->item_id == 27275 || held->item_id == 28547)) {
        calc->attack_roll += rc_player_effective_magic_attack_level(p)
                          * 2 * p->equipment_bonuses[EQ_MAGIC_ATK];
        calc->hit_chance = rc_hit_chance(calc->attack_roll, calc->defence_roll);
        int bonus = p->equipment_bonuses[EQ_MAGIC_DMG] * 3;
        if (bonus > 100) bonus = 100;
        bonus += rc_prayer_magic_damage_bonus(p->active_prayers);
        calc->max_hit = max_hit + max_hit * bonus / 100;
    }
    return 1;
}

int rc_content_player_hit_delay(const RcWorld *world, const RcNpc *target,
                                const RcSpellDef *spell, int hit_index) {
    const RcPlayer *p = world->player;
    const RcNpcDef *npc = rc_npc_def_for_npc(world, target);
    int size = npc && npc->size > 0 ? npc->size : 1;
    int dx = p->x - (target->x + size / 2), dy = p->y - (target->y + size / 2);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int distance = dx > dy ? dx : dy;
    const RcItemDef *weapon = rc_item_def_get(p->equipment[EQUIP_WEAPON].item_id);
    // Discrete server arrival rules, independent of any projectile assets.
    if (p->combat_style == COMBAT_MAGIC) {
        if (spell && !strcmp(spell->name, "Crumble Undead")) return 2 + (distance + 3) / 6;
        if (!spell && weapon && (weapon->id == 27275 || weapon->id == 28547))
            return 3 + (distance + 1) / 3;
        return 2 + (distance + 1) / 3;
    }
    if (p->combat_style != COMBAT_RANGED || (weapon && weapon->weapon_type == 26)) return 1;
    if (weapon && strstr(weapon->name, "Dark bow") && hit_index)
        return 2 + (2 * distance + 5) / 6;
    if (weapon && (weapon->weapon_type == 23 || weapon->weapon_type == 7))
        return 2 + distance / 6;
    return 2 + (distance + 3) / 6;
}

void rc_content_magic_hit(RcWorld *world, RcNpc *target,
                          const RcPendingHit *hit, int damage) {
    if (!hit->accurate || hit->attack_style != COMBAT_MAGIC) return;
    const RcSpellDef *spell = hit->spell_key ? rc_spell_def_get(hit->spell_key - 1) : NULL;
    int heal = 0;
    if (spell) {
        if (spell->effect_flags & RC_SPELL_EFFECT_HEAL) heal = damage / 4;
        if ((spell->effect_flags & RC_SPELL_EFFECT_POISON)
                && rc_npc_apply_poison(world, target, strstr(spell->name, "Blitz") ||
                    strstr(spell->name, "Barrage") ? 4 : 2))
            if (!target->player_damage[world->player->index]) target->player_damage[world->player->index] = 1;
        if (spell->effect_flags & RC_SPELL_EFFECT_FREEZE) {
            int ticks = !strcmp(spell->name, "Bind") ? 8 :
                !strcmp(spell->name, "Snare") ? 16 :
                !strcmp(spell->name, "Entangle") ? 24 :
                !strcmp(spell->name, "Ice Rush") ? 8 :
                !strcmp(spell->name, "Ice Burst") ? 16 :
                !strcmp(spell->name, "Ice Blitz") ? 24 :
                !strcmp(spell->name, "Ice Barrage") ? 32 :
                !strcmp(spell->name, "Undead Grasp") ? 4 :
                !strcmp(spell->name, "Skeletal Grasp") ? 2 : 1;
            bool grasp = strstr(spell->name, "Grasp") != NULL;
            if (world->tick >= target->immobilize_immune_until &&
                    (!grasp || rc_rng_range(&world->rng_state, 2) == 0)) {
                target->immobilized_until = world->tick + ticks;
                target->immobilize_immune_until = target->immobilized_until + 5;
            }
        }
        if (spell->effect_flags & RC_SPELL_EFFECT_DRAIN) {
            int stat = strstr(spell->name, "Shadow") || !strcmp(spell->name, "Confuse") ||
                !strcmp(spell->name, "Stun") ? 0 :
                !strcmp(spell->name, "Weaken") || !strcmp(spell->name, "Enfeeble") ? 2 :
                !strcmp(spell->name, "Flames of Zamorak") ? 5 : 1;
            int percent = !strcmp(spell->name, "Confuse") || !strcmp(spell->name, "Weaken") ||
                !strcmp(spell->name, "Curse") ? 5 : 10;
            if (!strcmp(spell->name, "Shadow Blitz") || !strcmp(spell->name, "Shadow Barrage"))
                percent = 15;
            const RcNpcDef *def = rc_npc_def_for_npc(world, target);
            int floor = def ? def->stats[stat] * (100 - percent) / 100 : 0;
            if (target->stats[stat] > floor) target->stats[stat] = floor;
        }
    } else if ((hit->weapon_id == 22323 || hit->weapon_id == 25731) &&
            rc_rng_range(&world->rng_state, 5) == 0) heal = damage / 2;
    if (heal > 0 && world->player->current_hp < world->player->max_hp) {
        RcPlayer *p = world->player;
        p->current_hp += heal * 10;
        if (p->current_hp > p->max_hp) p->current_hp = p->max_hp;
    }
}
