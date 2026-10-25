#include "prayer.h"
#include "io.h"
#include "player_actions.h"
#include "player_command.h"
#include "varbits.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PRAY_MAGIC 0x59415250u
#define PRAY_VERSION 2u
#define PRAYER_RAPID_RESTORE (1u << RC_PRAYER_RAPID_RESTORE)
#define PRAYER_RAPID_HEAL (1u << RC_PRAYER_RAPID_HEAL)
#define PRAYER_PRESERVE (1u << RC_PRAYER_PRESERVE)

RcPrayerDef g_rc_prayer_defs[RC_MAX_PRAYER_DEFS];
int g_rc_prayer_count = 0;

static const RcPrayerDef *g_active_prayer_defs = g_rc_prayer_defs;
static int g_active_prayer_count = 0;

void rc_prayer_use_defs(const RcPrayerDef *defs, int count) {
    g_active_prayer_defs = defs ? defs : g_rc_prayer_defs;
    g_active_prayer_count = defs ? count : 0;
}

void rc_prayer_reset_defs_if_active(const RcPrayerDef *defs) {
    if (defs && g_active_prayer_defs == defs)
        rc_prayer_use_defs(g_rc_prayer_defs, g_rc_prayer_count);
}

int rc_load_prayers_into(const char *path, RcPrayerDef *defs, int max_defs,
                         int *out_count) {
    if (out_count) *out_count = 0;
    if (!path || !defs || max_defs <= 0) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;

    RcPrayerDef staged[RC_MAX_PRAYER_DEFS] = {0};
    uint32_t magic, version, count;
    const char *error = "invalid header (PRAY v2 required; rebuild prayer data)";
    if (!rc_read_exact(f, &magic, sizeof(magic), 1, path, "magic")
            || !rc_read_exact(f, &version, sizeof(version), 1, path, "version")
            || !rc_read_exact(f, &count, sizeof(count), 1, path, "count")
            || magic != PRAY_MAGIC || version != PRAY_VERSION
            || count == 0 || count > RC_MAX_PRAYER_DEFS || count > (uint32_t)max_defs)
        goto fail;

    for (uint32_t i = 0; i < count; i++) {
        RcPrayerDef row = {0};
        uint8_t name_len;
        error = "truncated row";
        if (!rc_read_exact(f, &row.id, 1, 1, path, "id")
                || !rc_read_exact(f, &row.level, 1, 1, path, "level")
                || !rc_read_exact(f, &row.drain, 1, 1, path, "drain")
                || !rc_read_exact(f, &row.flags, 1, 1, path, "flags")
                || !rc_read_exact(f, &row.unlock_varbit, 2, 1, path, "unlock")
                || !rc_read_exact(f, &row.groups, 2, 1, path, "groups")
                || !rc_read_exact(f, &row.unlock_value, 1, 1, path, "unlock value")
                || !rc_read_exact(f, &row.defence_level, 1, 1, path, "defence level")
                || !rc_read_exact(f, &row.replaces_id, 1, 1, path, "replacement")
                || !rc_read_exact(f, &row.attack, 1, 1, path, "attack")
                || !rc_read_exact(f, &row.strength, 1, 1, path, "strength")
                || !rc_read_exact(f, &row.defence, 1, 1, path, "defence")
                || !rc_read_exact(f, &row.ranged_attack, 1, 1, path, "ranged attack")
                || !rc_read_exact(f, &row.ranged_strength, 1, 1, path, "ranged strength")
                || !rc_read_exact(f, &row.magic_attack, 1, 1, path, "magic attack")
                || !rc_read_exact(f, &row.magic_defence, 1, 1, path, "magic defence")
                || !rc_read_exact(f, &row.magic_damage, 1, 1, path, "magic damage")
                || !rc_read_exact(f, &name_len, 1, 1, path, "name length"))
            goto fail;
        error = "duplicate/unaddressable id or invalid prayer fields";
        if (row.id >= RC_MAX_PRAYER_DEFS || row.id >= max_defs
                || staged[row.id].loaded || !row.level || row.level > 99
                || !row.drain || row.defence_level > 99
                || (row.flags & ~7u) || (row.groups & ~15u)
                || row.unlock_varbit >= RC_MAX_VARBITS
                || (!!row.unlock_varbit != !!row.unlock_value)
                || !name_len || name_len >= sizeof(row.name)
                || row.attack < 0 || row.strength < 0 || row.defence < 0
                || row.ranged_attack < 0 || row.ranged_strength < 0
                || row.magic_attack < 0 || row.magic_defence < 0 || row.magic_damage < 0)
            goto fail;
        error = "truncated/invalid name";
        if (!rc_read_exact(f, row.name, 1, name_len, path, "name")
                || memchr(row.name, 0, name_len))
            goto fail;
        row.loaded = 1;
        staged[row.id] = row;
    }
    error = "invalid or duplicate replacement reference";
    uint32_t replaced = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *p = &staged[i];
        if (!p->loaded || p->replaces_id == 255) continue;
        int target = p->replaces_id;
        if (target >= RC_MAX_PRAYER_DEFS || target == i || !staged[target].loaded
                || staged[target].replaces_id != 255 || (replaced & (1u << target))
                || p->level < staged[target].level || p->groups != staged[target].groups)
            goto fail;
        replaced |= 1u << target;
    }
    error = "unexpected trailing bytes";
    if (fgetc(f) != EOF || ferror(f)) goto fail;
    rc_asset_close(f);
    memset(defs, 0, (size_t)max_defs * sizeof(*defs));
    int copy_count = max_defs < RC_MAX_PRAYER_DEFS ? max_defs : RC_MAX_PRAYER_DEFS;
    memcpy(defs, staged, (size_t)copy_count * sizeof(*defs));
    if (out_count) *out_count = (int)count;
    return (int)count;
fail:
    fprintf(stderr, "prayers: %s: %s\n", path, error);
    rc_asset_close(f);
    return -1;
}

int rc_load_prayers(const char *path) {
    int loaded = 0;
    int result = rc_load_prayers_into(path, g_rc_prayer_defs,
                                      RC_MAX_PRAYER_DEFS, &loaded);
    if (result >= 0) {
        g_rc_prayer_count = loaded;
        rc_prayer_use_defs(g_rc_prayer_defs, loaded);
    }
    return result;
}

const RcPrayerDef *rc_prayer_def_get(int prayer_id) {
    if (prayer_id < 0 || prayer_id >= RC_MAX_PRAYER_DEFS) return NULL;
    return g_active_prayer_defs[prayer_id].loaded
        ? &g_active_prayer_defs[prayer_id] : NULL;
}

uint32_t rc_prayer_bit(int prayer_id) {
    return prayer_id >= 0 && prayer_id < RC_MAX_PRAYER_DEFS ? 1u << prayer_id : 0;
}

static RcPrayerResult requirements(const RcWorld *world, const RcPrayerDef *def) {
    if (!world || !def) return RC_PRAYER_INVALID;
    if (!rc_player_action_allowed(world->enabled, RC_PLAYER_ACTION_SET_PRAYER))
        return RC_PRAYER_DISABLED;
    if (def->flags & RC_PRAYER_EFFECT_UNAVAILABLE) return RC_PRAYER_UNSUPPORTED;
    if ((def->flags & RC_PRAYER_MEMBERS_ONLY) && !world->members_world)
        return RC_PRAYER_MEMBERS;
    if (world->player.skills.base_level[SKILL_PRAYER] < def->level)
        return RC_PRAYER_LEVEL;
    if (world->player.skills.base_level[SKILL_DEFENCE] < def->defence_level)
        return RC_PRAYER_DEFENCE;
    if (def->unlock_varbit) {
        if (!rc_varbit_def_get(def->unlock_varbit)) return RC_PRAYER_MISSING_DATA;
        if (rc_varbit_get(world, def->unlock_varbit) < def->unlock_value)
            return RC_PRAYER_LOCKED;
    }
    return RC_PRAYER_OK;
}

int rc_prayer_resolve(const RcWorld *world, int prayer_id) {
    if (!rc_prayer_def_get(prayer_id)) return -1;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if (def && def->replaces_id == prayer_id && requirements(world, def) == RC_PRAYER_OK)
            return i;
    }
    return prayer_id;
}

RcPrayerResult rc_prayer_available(const RcWorld *world, int prayer_id) {
    RcPrayerResult result = requirements(world, rc_prayer_def_get(prayer_id));
    if (result != RC_PRAYER_OK) return result;
    return rc_prayer_resolve(world, prayer_id) == prayer_id ? RC_PRAYER_OK : RC_PRAYER_REPLACED;
}

int rc_prayer_result_accepted(RcPrayerResult result) {
    return result == RC_PRAYER_OK || result == RC_PRAYER_QUEUED;
}

const char *rc_prayer_result_message(RcPrayerResult result) {
    switch (result) {
    case RC_PRAYER_OK: case RC_PRAYER_QUEUED: return "";
    case RC_PRAYER_INVALID: return "Unknown prayer.";
    case RC_PRAYER_DISABLED: return "Prayer is disabled in this world.";
    case RC_PRAYER_DEAD: return "You cannot use prayers while dead.";
    case RC_PRAYER_EMPTY: return "You need Prayer points. Recharge at an altar.";
    case RC_PRAYER_LEVEL: return "Your base Prayer level is too low for this prayer.";
    case RC_PRAYER_DEFENCE: return "Your base Defence level is too low for this prayer.";
    case RC_PRAYER_LOCKED: return "You have not unlocked this prayer.";
    case RC_PRAYER_MEMBERS: return "This prayer requires a members world.";
    case RC_PRAYER_UNSUPPORTED: return "This prayer's effect is not implemented yet.";
    case RC_PRAYER_MISSING_DATA: return "Prayer unlock definitions are missing.";
    case RC_PRAYER_NO_SELECTION: return "You have not selected any quick prayers.";
    case RC_PRAYER_REPLACED: return "An unlocked prayer replaces this prayer.";
    case RC_PRAYER_CONFLICT: return "These prayers cannot be active together.";
    case RC_PRAYER_QUEUE_REJECTED: return "Prayer command could not be queued.";
    case RC_PRAYER_DEPLETED: return "You have run out of Prayer points. Recharge at an altar.";
    case RC_PRAYER_FORCED_OFF: return "Your prayers have been disabled.";
    }
    return "Invalid prayer result.";
}

static RcPrayerResult outcome(RcWorld *world, int id, RcPrayerResult code) {
    if (world) {
        RcPrayerOutcome *result = &world->player.prayer_outcome;
        result->sequence++;
        result->prayer_id = id;
        result->code = code;
    }
    return code;
}

static void commit_active(RcWorld *world, uint32_t mask) {
    RcPlayer *p = &world->player;
    uint32_t old = p->active_prayers;
    uint32_t changed = old ^ mask;
    // -1 skips stat timer accumulation in this activation/disable tick.
    if (changed & PRAYER_RAPID_HEAL) p->hp_regen_counter = -1;
    if (changed & PRAYER_RAPID_RESTORE)
        p->rapid_restore_counter = mask & PRAYER_RAPID_RESTORE ? -1 : 0;
    if (changed & PRAYER_PRESERVE) {
        p->preserve_timer = mask & PRAYER_PRESERVE ? 26 : 0;
        if (!(mask & PRAYER_PRESERVE)) p->stat_boost_counter = -1;
    }
    if (!old && mask) p->prayer_drain_start_tick = world->tick + 1;
    if (!mask) {
        p->prayer_drain_counter = 0;
        p->prayer_drain_start_tick = 0;
    }
    p->quick_prayers_active = false;
    p->active_prayers = mask;
    for (int i = 0; changed && i < RC_MAX_PRAYER_DEFS; i++) {
        uint32_t bit = rc_prayer_bit(i);
        if (!(changed & bit)) continue;
        changed &= ~bit;
        RcPayloadPrayerToggled payload = {(uint16_t)i, (mask & bit) != 0};
        rc_event_fire(world, RC_EVT_PRAYER_TOGGLED, &payload);
    }
}

void rc_prayer_disable_all(RcWorld *world, RcPrayerResult reason) {
    if (!world) return;
    bool active = world->player.active_prayers || world->player.quick_prayers_active;
    commit_active(world, 0);
    if (active) outcome(world, -1, reason);
}

int rc_prayer_cap_tenths(const RcPlayer *p) {
    if (!p || p->skills.base_level[SKILL_PRAYER] <= 0) return 0;
    int64_t cap = (int64_t)p->skills.base_level[SKILL_PRAYER] * RC_PRAYER_POINT_SCALE;
    return cap > INT_MAX ? INT_MAX : (int)cap;
}

void rc_prayer_recharge(RcPlayer *p) {
    if (!p) return;
    p->current_prayer_points = rc_prayer_cap_tenths(p);
    p->prayer_drain_counter = 0;
}

void rc_prayer_reset(RcPlayer *p) {
    if (!p) return;
    p->active_prayers = 0;
    p->quick_prayers_active = false;
    p->prayer_drain_start_tick = 0;
    p->rapid_restore_counter = p->preserve_timer = 0;
    rc_prayer_recharge(p);
}

void rc_prayer_drain_tenths(RcWorld *world, int tenths) {
    if (!world || tenths <= 0) return;
    RcPlayer *p = &world->player;
    if (p->current_prayer_points <= 0) return;
    p->current_prayer_points = tenths >= p->current_prayer_points
        ? 0 : p->current_prayer_points - tenths;
    if (!p->current_prayer_points) {
        commit_active(world, 0);
        outcome(world, -1, RC_PRAYER_DEPLETED);
    }
}

void rc_prayer_drain_points(RcWorld *world, int points) {
    if (points <= 0) return;
    int64_t tenths = (int64_t)points * RC_PRAYER_POINT_SCALE;
    rc_prayer_drain_tenths(world, tenths > INT_MAX ? INT_MAX : (int)tenths);
}

void rc_prayer_restore_points(RcPlayer *p, int points) {
    if (!p || points <= 0) return;
    int cap = rc_prayer_cap_tenths(p);
    if (p->current_prayer_points >= cap) return;
    int current = p->current_prayer_points > 0 ? p->current_prayer_points : 0;
    int64_t restored = current + (int64_t)points * RC_PRAYER_POINT_SCALE;
    p->current_prayer_points = restored >= cap ? cap : (int)restored;
}

static uint32_t select_prayer(uint32_t mask, const RcPrayerDef *def) {
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *other = rc_prayer_def_get(i);
        if (other && (other->groups & def->groups)) mask &= ~rc_prayer_bit(i);
    }
    return mask | rc_prayer_bit(def->id);
}

RcPrayerResult rc_prayer_set_active(RcWorld *world, uint32_t mask) {
    if (!world) return RC_PRAYER_INVALID;
    if (mask && (world->player.is_dead || world->player.current_hp <= 0))
        return outcome(world, -1, RC_PRAYER_DEAD);
    if (mask && world->player.current_prayer_points <= 0)
        return outcome(world, -1, RC_PRAYER_EMPTY);
    uint16_t groups = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        if (!(mask & rc_prayer_bit(i))) continue;
        RcPrayerResult result = rc_prayer_available(world, i);
        if (result != RC_PRAYER_OK) return outcome(world, i, result);
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if (groups & def->groups) return outcome(world, i, RC_PRAYER_CONFLICT);
        groups |= def->groups;
    }
    commit_active(world, mask);
    return outcome(world, -1, RC_PRAYER_OK);
}

static RcPrayerResult queue_prayer(RcWorld *world, RcPlayerCommandKind kind, int id) {
    int args[8] = {id};
    if (!rc_player_command_submit(world, kind, RC_ACTION_CATEGORY_SOFT, args, 0)) {
        RcPrayerResult code = world->player_commands.last_result == RC_COMMAND_RESULT_REJECTED_DEAD
            ? RC_PRAYER_DEAD : RC_PRAYER_QUEUE_REJECTED;
        return outcome(world, id, code);
    }
    return outcome(world, id, RC_PRAYER_QUEUED);
}

RcPrayerResult rc_player_set_prayer(RcWorld *world, int id) {
    if (!world) return RC_PRAYER_INVALID;
    if (rc_player_command_should_queue(world))
        return queue_prayer(world, RC_PLAYER_COMMAND_SET_PRAYER, id);
    uint32_t bit = rc_prayer_bit(id);
    RcPlayer *p = &world->player;
    if (bit && (p->active_prayers & bit)) {
        commit_active(world, p->active_prayers & ~bit);
        return outcome(world, id, RC_PRAYER_OK);
    }
    RcPrayerResult result = rc_prayer_available(world, id);
    if (result != RC_PRAYER_OK) return outcome(world, id, result);
    result = rc_prayer_set_active(world, select_prayer(p->active_prayers, rc_prayer_def_get(id)));
    return outcome(world, id, result);
}

RcPrayerResult rc_player_select_quick_prayer(RcWorld *world, int id) {
    if (!world) return RC_PRAYER_INVALID;
    if (rc_player_command_should_queue(world))
        return queue_prayer(world, RC_PLAYER_COMMAND_SELECT_QUICK_PRAYER, id);
    RcPlayer *p = &world->player;
    uint32_t bit = rc_prayer_bit(id);
    if (bit && (p->quick_prayers & bit)) {
        p->quick_prayers &= ~bit;
    } else {
        RcPrayerResult result = rc_prayer_available(world, id);
        if (result != RC_PRAYER_OK) return outcome(world, id, result);
        p->quick_prayers = select_prayer(p->quick_prayers, rc_prayer_def_get(id));
    }
    return outcome(world, id, RC_PRAYER_OK);
}

RcPrayerResult rc_player_toggle_quick_prayers(RcWorld *world) {
    if (!world) return RC_PRAYER_INVALID;
    if (rc_player_command_should_queue(world))
        return queue_prayer(world, RC_PLAYER_COMMAND_TOGGLE_QUICK_PRAYERS, -1);
    RcPlayer *p = &world->player;
    if (p->quick_prayers_active) {
        rc_prayer_disable_all(world, RC_PRAYER_OK);
        return RC_PRAYER_OK;
    }
    if (!p->quick_prayers) return outcome(world, -1, RC_PRAYER_NO_SELECTION);
    RcPrayerResult result = rc_prayer_set_active(world, p->quick_prayers);
    if (result == RC_PRAYER_OK) p->quick_prayers_active = true;
    return result;
}

void rc_prayer_drain_tick(RcWorld *world) {
    if (!world) return;
    RcPlayer *p = &world->player;
    if (!p->active_prayers) return;
    if (p->is_dead || p->current_hp <= 0) {
        rc_prayer_disable_all(world, RC_PRAYER_DEAD);
        return;
    }
    if (p->current_prayer_points <= 0) {
        rc_prayer_disable_all(world, RC_PRAYER_DEPLETED);
        return;
    }
    if (world->tick < p->prayer_drain_start_tick) return;
    int rate = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        if (!(p->active_prayers & rc_prayer_bit(i))) continue;
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if (def) rate += def->drain;
    }
    int64_t resistance = 60 + 2 * (int64_t)p->equipment_bonuses[13];
    if (resistance < 1) resistance = 1;
    int64_t counter = (p->prayer_drain_counter > 0 ? p->prayer_drain_counter : 0) + (int64_t)rate;
    int64_t points = counter > 0 ? (counter - 1) / resistance : 0;
    p->prayer_drain_counter = counter - points * resistance;
    if (points > 0)
        rc_prayer_drain_points(world, points > INT_MAX ? INT_MAX : (int)points);
}

int rc_prayer_attack_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def && def->attack > best) {
            best = def->attack;
        }
    }
    return best;
}

int rc_prayer_strength_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def && def->strength > best) {
            best = def->strength;
        }
    }
    return best;
}

int rc_prayer_defence_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int total = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def) {
            total += def->defence;
        }
    }
    return total;
}

int rc_prayer_ranged_attack_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def
                && def->ranged_attack > best) {
            best = def->ranged_attack;
        }
    }
    return best;
}

int rc_prayer_ranged_strength_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def
                && def->ranged_strength > best) {
            best = def->ranged_strength;
        }
    }
    return best;
}

int rc_prayer_magic_attack_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def
                && def->magic_attack > best) {
            best = def->magic_attack;
        }
    }
    return best;
}

int rc_prayer_magic_damage_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def
                && def->magic_damage > best) {
            best = def->magic_damage;
        }
    }
    return best;
}

int rc_prayer_magic_defence_bonus(uint32_t active_prayers) {
    if (!active_prayers) return 0;
    int best = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if ((active_prayers & rc_prayer_bit(i)) && def && def->magic_defence > best)
            best = def->magic_defence;
    }
    return best;
}
