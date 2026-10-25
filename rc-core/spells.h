#ifndef RC_SPELLS_H
#define RC_SPELLS_H

#include <stdint.h>

#define RC_MAX_SPELL_DEFS 512
#define RC_SPELL_MAX_RUNES 8

enum {
    RC_SPELL_BOOK_STANDARD = 0,
    RC_SPELL_BOOK_ANCIENT = 1,
    RC_SPELL_BOOK_LUNAR = 2,
    RC_SPELL_BOOK_ARCEUUS = 3,
    RC_SPELL_BOOK_ALL = 4,
};

enum {
    RC_SPELL_TYPE_UNSUPPORTED = 0,
    RC_SPELL_TYPE_COMBAT = 1,
    RC_SPELL_TYPE_TELEPORT = 2,
    RC_SPELL_TYPE_UTILITY = 3,
    RC_SPELL_TYPE_SKILLING = 4,
    RC_SPELL_TYPE_CURSE = 5,
    RC_SPELL_TYPE_CHARGING = 6,
    RC_SPELL_TYPE_SUMMONING = 7,
};

enum {
    RC_SPELL_EFFECT_DAMAGE = 1u << 0,
    RC_SPELL_EFFECT_FREEZE = 1u << 1,
    RC_SPELL_EFFECT_HEAL = 1u << 2,
    RC_SPELL_EFFECT_POISON = 1u << 3,
    RC_SPELL_EFFECT_DRAIN = 1u << 4,
    RC_SPELL_EFFECT_TELEPORT = 1u << 5,
};

typedef struct {
    uint32_t item_id;
    uint8_t qty;
} RcSpellRune;

typedef struct RcSpellDef {
    char name[64];
    uint8_t book, type, level, slayer_level, flags, rune_count;
    uint16_t xp_q1, max_hit, effect_flags;
    RcSpellRune runes[RC_SPELL_MAX_RUNES];
    uint8_t loaded;
} RcSpellDef;

extern RcSpellDef g_rc_spell_defs[RC_MAX_SPELL_DEFS];
extern int g_rc_spell_count;

int rc_load_spells(const char *path);
int rc_load_spells_into(const char *path, RcSpellDef *defs, int max_defs,
                        int *out_count);
void rc_spell_use_defs(const RcSpellDef *defs, int count);
void rc_spell_reset_defs_if_active(const RcSpellDef *defs);
const RcSpellDef *rc_spell_def_get(int spell_idx);
int rc_spell_find(const char *name);

struct RcWorld;
typedef enum {
    RC_SPELL_OK, RC_SPELL_QUEUED, RC_SPELL_INVALID, RC_SPELL_DISABLED,
    RC_SPELL_WRONG_BOOK, RC_SPELL_LEVEL, RC_SPELL_MEMBERS, RC_SPELL_RUNES,
    RC_SPELL_WEAPON, RC_SPELL_UNSUPPORTED, RC_SPELL_QUEUE_FULL, RC_SPELL_DEAD,
} RcSpellResult;
RcSpellResult rc_spell_available(const struct RcWorld *world, int spell_idx, int autocast);
int rc_spell_result_accepted(RcSpellResult result);
const char *rc_spell_result_message(RcSpellResult result);
int rc_spell_is_combat(const RcSpellDef *spell);

#endif
