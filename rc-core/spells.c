#include "spells.h"
#include "io.h"
#include "combat.h"
#include "combat_formula.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define SPEL_MAGIC 0x4C455053u
#define TELE_MAGIC 0x454C4554u
#define SPEL_V1 1u
#define SPEL_V2 2u

RcSpellDef g_rc_spell_defs[RC_MAX_SPELL_DEFS];
int g_rc_spell_count = 0;

static const RcSpellDef *g_active_spell_defs = g_rc_spell_defs;
static int g_active_spell_count = 0;

void rc_spell_use_defs(const RcSpellDef *defs, int count) {
    g_active_spell_defs = defs ? defs : g_rc_spell_defs;
    g_active_spell_count = defs ? count : 0;
}

void rc_spell_reset_defs_if_active(const RcSpellDef *defs) {
    if (defs && g_active_spell_defs == defs) {
        rc_spell_use_defs(g_rc_spell_defs, g_rc_spell_count);
    }
}

static int read_name(FILE *f, RcSpellDef *row, const char *path) {
    uint8_t len;
    if (!rc_read_exact(f, &len, sizeof(len), 1, path, "spell name len")) {
        return 0;
    }
    if (!len || len >= sizeof(row->name)) return 0;
    uint8_t keep = len;
    if (keep && !rc_read_exact(f, row->name, 1, keep, path, "spell name")) {
        return 0;
    }
    row->name[keep] = '\0';
    if (strlen(row->name) != len) return 0;
    return 1;
}

int rc_load_spells_into(const char *path, RcSpellDef *defs, int max_defs,
                        int *out_count) {
    if (!path || !defs || max_defs <= 0) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version, count;
    if (!rc_read_exact(f, &magic, sizeof(magic), 1, path, "magic")
            || !rc_read_exact(f, &version, sizeof(version), 1, path, "version")
            || !rc_read_exact(f, &count, sizeof(count), 1, path, "count")
            || (magic != SPEL_MAGIC && magic != TELE_MAGIC)
            || (version != SPEL_V1 && version != SPEL_V2)) {
        rc_asset_close(f);
        return -1;
    }

    if (count > (uint32_t)max_defs || count > RC_MAX_SPELL_DEFS) {
        fprintf(stderr, "spells: %s: row count %u exceeds capacity\n", path, count);
        rc_asset_close(f);
        return -1;
    }
    RcSpellDef *staged = calloc((size_t)max_defs, sizeof(*staged));
    if (!staged) { rc_asset_close(f); return -1; }
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        RcSpellDef row;
        memset(&row, 0, sizeof(row));
        if (!read_name(f, &row, path)
                || !rc_read_exact(f, &row.book, sizeof(row.book), 1, path, "book")
                || !rc_read_exact(f, &row.type, sizeof(row.type), 1, path, "type")
                || !rc_read_exact(f, &row.level, sizeof(row.level), 1, path, "level")
                || !rc_read_exact(f, &row.slayer_level, sizeof(row.slayer_level),
                                  1, path, "slayer level")
                || !rc_read_exact(f, &row.xp_q1, sizeof(row.xp_q1), 1, path, "xp")
                || !rc_read_exact(f, &row.flags, sizeof(row.flags), 1, path, "flags")) {
            goto invalid;
        }
        if (version >= SPEL_V2) {
            if (!rc_read_exact(f, &row.max_hit, sizeof(row.max_hit), 1, path, "max hit")
                    || !rc_read_exact(f, &row.effect_flags, sizeof(row.effect_flags),
                                      1, path, "effect flags")) {
                goto invalid;
            }
        }
        uint8_t rune_count;
        if (!rc_read_exact(f, &rune_count, sizeof(rune_count), 1, path, "rune count")) {
            goto invalid;
        }
        if (rune_count > RC_SPELL_MAX_RUNES || row.book > RC_SPELL_BOOK_ALL
                || row.type > RC_SPELL_TYPE_SUMMONING || !row.level || row.level > 99
                || row.slayer_level > 99 || row.flags > 1 || (row.effect_flags & ~63u))
            goto invalid;
        for (int j = 0; j < loaded; j++)
            if (staged[j].book == row.book && !strcmp(staged[j].name, row.name)) goto invalid;
        row.rune_count = rune_count;
        for (uint8_t r = 0; r < rune_count; r++) {
            uint32_t item_id;
            uint8_t qty;
            if (!rc_read_exact(f, &item_id, sizeof(item_id), 1, path, "rune item")
                    || !rc_read_exact(f, &qty, sizeof(qty), 1, path, "rune qty")) {
                goto invalid;
            }
            if (!item_id || item_id > INT_MAX || !qty) goto invalid;
            for (int j = 0; j < r; j++) if (row.runes[j].item_id == item_id) goto invalid;
            row.runes[r] = (RcSpellRune){item_id, qty};
        }
        row.loaded = 1;
        staged[loaded++] = row;
    }
    if (fgetc(f) != EOF || ferror(f)) goto invalid;
    rc_asset_close(f);
    memcpy(defs, staged, (size_t)max_defs * sizeof(*defs));
    free(staged);
    if (out_count) *out_count = loaded;
    return loaded;
invalid:
    fprintf(stderr, "spells: %s: invalid/truncated row %d (name, fields, or rune requirements); previous definitions retained\n",
            path, loaded);
    free(staged);
    rc_asset_close(f);
    return -1;
}

int rc_load_spells(const char *path) {
    int loaded = 0;
    int result = rc_load_spells_into(path, g_rc_spell_defs, RC_MAX_SPELL_DEFS,
                                     &loaded);
    if (result >= 0) {
        g_rc_spell_count = loaded;
        rc_spell_use_defs(g_rc_spell_defs, g_rc_spell_count);
    }
    return result;
}

const RcSpellDef *rc_spell_def_get(int spell_idx) {
    const RcSpellDef *defs = g_active_spell_defs
                           ? g_active_spell_defs : g_rc_spell_defs;
    int count = defs == g_rc_spell_defs ? g_rc_spell_count
                                        : g_active_spell_count;
    if (spell_idx >= 0 && spell_idx < count && defs[spell_idx].loaded) {
        return &defs[spell_idx];
    }
    return NULL;
}

int rc_spell_find(const char *name) {
    if (!name) return -1;
    const RcSpellDef *defs = g_active_spell_defs
                           ? g_active_spell_defs : g_rc_spell_defs;
    int count = defs == g_rc_spell_defs ? g_rc_spell_count
                                        : g_active_spell_count;
    for (int i = 0; i < count; i++) {
        if (strcmp(defs[i].name, name) == 0) return i;
    }
    return -1;
}

int rc_spell_is_combat(const RcSpellDef *spell) {
    return spell && spell->loaded && spell->type == RC_SPELL_TYPE_COMBAT
        && (spell->max_hit || (spell->effect_flags & (RC_SPELL_EFFECT_FREEZE | RC_SPELL_EFFECT_DRAIN)));
}

RcSpellResult rc_spell_available(const RcWorld *world, int spell_idx, int autocast) {
    if (!world) return RC_SPELL_INVALID;
    if (!(world->enabled & RC_SUB_COMBAT)) return RC_SPELL_DISABLED;
    const RcSpellDef *spell = rc_spell_def_get(spell_idx);
    if (!spell) return RC_SPELL_INVALID;
    const RcPlayer *p = &world->player;
    if (p->is_dead || p->current_hp <= 0) return RC_SPELL_DEAD;
    if (spell->book != p->current_spellbook && spell->book != RC_SPELL_BOOK_ALL)
        return RC_SPELL_WRONG_BOOK;
    if (spell->type == RC_SPELL_TYPE_UNSUPPORTED) return RC_SPELL_UNSUPPORTED;
    if ((spell->flags & 1) && !world->members_world) return RC_SPELL_MEMBERS;
    if (p->skills.boosted_level[SKILL_MAGIC] < spell->level ||
        p->skills.boosted_level[SKILL_SLAYER] < spell->slayer_level) return RC_SPELL_LEVEL;
    if (autocast && (!rc_spell_is_combat(spell) || !spell->max_hit ||
        !rc_player_weapon_can_autocast(p) || (world->combat_hooks.can_autocast_spell &&
        !world->combat_hooks.can_autocast_spell(p, spell)))) return RC_SPELL_WEAPON;
    if (!rc_combat_has_spell_runes(world, p, spell)) return RC_SPELL_RUNES;
    return RC_SPELL_OK;
}

int rc_spell_result_accepted(RcSpellResult result) {
    return result == RC_SPELL_OK || result == RC_SPELL_QUEUED;
}

const char *rc_spell_result_message(RcSpellResult result) {
    static const char *const messages[] = {
        "", "", "That spell is unavailable.", "Magic actions are disabled.",
        "That spell belongs to another spellbook.", "Your Magic or Slayer level is too low for this spell.",
        "This spell requires a members world.", "You do not have enough runes or valid source charges to cast this spell.",
        "This weapon cannot autocast that spell.", "This spell has no supported action.",
        "The command queue is full; spell action rejected.",
        "You cannot use spells while dead.",
    };
    return (unsigned)result < sizeof(messages) / sizeof(messages[0]) ? messages[result] : messages[RC_SPELL_INVALID];
}
