#ifndef RC_MODE_H
#define RC_MODE_H

// Gym mode rules: a mode's content whitelist and overrides, compiled by tools/modec from
// modes/<mode>/*.toml into data/modes/<mode>/mode.rules (one record per line, `kind key=value`).
//
// rc_game_data_load reads them when RcWorldConfig.mode_rules_path is set, applies the definition
// overrides (equipment requirements, NPC profiles, spells, prayers, the closed recipe table) while
// logging every change in a ledger, and keeps the rules active for the mode's content module
// (rc-content/compact.c) and the engine's mode extensions (XP multiplier and level cap).

#include "items.h"
#include "npc.h"
#include "prayer.h"
#include "skilling.h"
#include "skills.h"
#include "spells.h"
#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RC_MODE_MAX_START 32
#define RC_MODE_MAX_EQUIP 96
#define RC_MODE_MAX_TOOLS 8
#define RC_MODE_MAX_GATHER 32
#define RC_MODE_MAX_FISH 8
#define RC_MODE_MAX_COOK 16
#define RC_MODE_MAX_COOK_OBJECTS 16
#define RC_MODE_MAX_STATIONS 16
#define RC_MODE_MAX_RECIPES 96
#define RC_MODE_MAX_SPELLS 8
#define RC_MODE_MAX_PRAYERS 32
#define RC_MODE_MAX_BURY 4
#define RC_MODE_MAX_NPCS 16
#define RC_MODE_MAX_DROP_ROWS 8
#define RC_MODE_MAX_DROP_ROLLS 4
#define RC_MODE_MAX_SHOPS 4
#define RC_MODE_MAX_STOCK 64
#define RC_MODE_MAX_VALUES 192
#define RC_MODE_MAX_BONE 4
#define RC_MODE_MAX_RATS 8
#define RC_MODE_MAX_FOOD_PILES 4

typedef enum {
    RC_MODE_START_EQUIP = 0,
    RC_MODE_START_INVENTORY,
    RC_MODE_START_BANK,
    RC_MODE_START_KINDS,
} RcModeStartKind;

typedef struct {
    int item, qty;
} RcModeStack;

// Use requirements replacing an item's inherited ones (0: none in that skill).
typedef struct {
    int item;
    uint8_t level[SKILL_COUNT];
} RcModeEquip;

// A bone weapon: the precursor whose bonuses it takes, its rat bonus and powered-spell values.
typedef struct {
    int item, base;
    int rat_bonus;
    int powered_max, speed, charge_item;  // bone staff: fixed base max hit, ticks, charge rune
    int rats[RC_MODE_MAX_RATS];
    int rat_count;
} RcModeBoneWeapon;

typedef struct {
    int id;              // manifest recipe id = index in the mode's recipe table
    uint8_t station;     // RcMakeMenu
    bool batch;          // one action converts every carried input (rune altars)
    int loc;             // the station loc for altar recipes, else 0
    int canonical;       // recipes.bin row it came from, -1 for mode-authored rows
    RcRecipe recipe;
} RcModeRecipe;

typedef struct {
    char name[64];
    uint8_t level;
    uint16_t max_hit;
} RcModeSpell;

typedef struct {
    char name[64];
    uint8_t level;
} RcModePrayer;

typedef struct {
    int item, xp_tenths;
} RcModeBury;

enum { RC_ELEMENT_NONE = 0, RC_ELEMENT_AIR, RC_ELEMENT_WATER, RC_ELEMENT_EARTH, RC_ELEMENT_FIRE };

typedef struct {
    int id, hp, max_hit;
    bool has_defence;
    int16_t defence[5];  // stab, slash, crush, ranged, magic
    uint8_t weakness_element, weakness_pct;
} RcModeNpc;

typedef struct {
    int item, min, max, weight;
} RcModeDropRow;

typedef struct {
    int one_in;  // the roll happens with chance 1/one_in, then picks one row by weight
    RcModeDropRow pick[RC_MODE_MAX_DROP_ROWS];
    int count;
} RcModeDropRoll;

typedef struct {
    int npc;
    RcModeDropRow always[RC_MODE_MAX_DROP_ROWS];
    int always_count;
    RcModeDropRoll rolls[RC_MODE_MAX_DROP_ROLLS];
    int roll_count;
} RcModeDrop;

typedef struct {
    int item, price, cap, qty, ticks;
} RcModeStock;

typedef struct {
    char name[48];
    int npc;
    bool buys_valued;  // buys every item with a sale value (else only its own stock)
    RcModeStock stock[RC_MODE_MAX_STOCK];
    int stock_count;
} RcModeShop;

typedef struct {
    int item, value;
} RcModeValue;

typedef struct {
    int npc, minion;
    int hp, group_scale_pct, max_hit, attack_speed, final_attack_speed;
    int attack, defence, magic;
    int16_t defence_bonus[5];
    int hazard_max_hit, hazard_warning_ticks, hazard_interval;
    int minion_hp, minion_max_hit, minion_attack_speed, minions_per_summon, max_minions;
    int summon_interval, feed_heal, feed_heals, feed_ticks, feed_interval, hazard_random_tiles;
    int phase2_pct, phase3_pct, respawn_ticks, reset_ticks, qualify_pct;
    int rect[4], centre[2], spawn[2];  // dungeon-area tiles
    int food[RC_MODE_MAX_FOOD_PILES][2];
    int food_count;
    RcModeDropRow always[4];
    int always_count;
    RcModeStack bundle[4];
    int bundle_count;
    struct { int item, qty, one_in; } extra[4];
    int extra_count;
} RcModeBoss;

typedef struct RcModeRules {
    char name[64];
    int version, level_cap, xp_multiplier, max_players, horizon;
    bool members;
    uint32_t skill_mask;  // enabled skills (1 << RcSkill)
    int respawn_ticks, recovery_ticks, private_ticks, public_ticks, npc_respawn;
    RcModeStack start[RC_MODE_START_KINDS][RC_MODE_MAX_START];
    int start_count[RC_MODE_START_KINDS];
    RcModeEquip equip[RC_MODE_MAX_EQUIP];
    int equip_count;
    RcModeBoneWeapon bone[RC_MODE_MAX_BONE];
    int bone_count;
    RcSkillTool axes[RC_MODE_MAX_TOOLS], pickaxes[RC_MODE_MAX_TOOLS];  // best first
    int axe_count, pickaxe_count;
    RcGatherRule gather[RC_MODE_MAX_GATHER];
    int gather_count;
    RcFishRule fish[RC_MODE_MAX_FISH];
    int fish_count;
    RcCookRule cook[RC_MODE_MAX_COOK];
    int cook_count;
    uint32_t cook_objects[RC_MODE_MAX_COOK_OBJECTS];
    int cook_object_count;
    struct { uint8_t station; int loc; } stations[RC_MODE_MAX_STATIONS];
    int station_count;
    RcModeRecipe recipes[RC_MODE_MAX_RECIPES];
    int recipe_count;
    RcModeSpell spells[RC_MODE_MAX_SPELLS];
    int spell_count;
    RcModePrayer prayers[RC_MODE_MAX_PRAYERS];
    int prayer_count;
    RcModeBury bury[RC_MODE_MAX_BURY];
    int bury_count;
    RcModeNpc npcs[RC_MODE_MAX_NPCS];
    int npc_count;
    RcModeDrop drops[RC_MODE_MAX_NPCS];
    int drop_count;
    RcModeShop shops[RC_MODE_MAX_SHOPS];
    int shop_count;
    RcModeValue sale[RC_MODE_MAX_VALUES];
    int sale_count;
    RcModeValue purchase[8];
    int purchase_count;
    int tanner_npc, aldo_npc;
    bool has_boss;
    RcModeBoss boss;
} RcModeRules;

// Parses a rules file; returns 1, or 0 with a message naming the line.
int rc_mode_rules_load(const char *path, RcModeRules *out, char *err, size_t err_cap);


// ---- Override ledger -----------------------------------------------------------------------------

typedef struct {
    char kind[12];    // item, npc, spell, prayer, recipe
    int id;
    char name[64];
    char field[40];
    char from[96];
    char to[96];
} RcModeOverride;

typedef struct {
    RcModeOverride *rows;
    int count, cap;
} RcModeLedger;

void rc_mode_ledger_free(RcModeLedger *ledger);

// The rules of the loaded game data (NULL when it has none) and the ledger of the overrides they
// applied. Immutable while worlds run.
const RcModeRules *rc_mode_active(void);
void rc_mode_set_active(const RcModeRules *rules, const RcModeLedger *ledger);
// The ledger of the active rules' patch (NULL before a load with rules).
const RcModeLedger *rc_mode_active_ledger(void);

// Applies the rules to loaded definitions, logging each change (ledger may be NULL).
int rc_mode_patch_items(const RcModeRules *rules, RcItemDef *defs, int count, RcModeLedger *ledger);
int rc_mode_patch_npcs(const RcModeRules *rules, RcNpcDef *defs, int count, const int *by_id,
                       int by_id_count, RcModeLedger *ledger);
int rc_mode_patch_spells(const RcModeRules *rules, struct RcSpellDef *defs, int count,
                         RcModeLedger *ledger);
int rc_mode_patch_prayers(const RcModeRules *rules, RcPrayerDef *defs, int count,
                          RcModeLedger *ledger);
// Replaces the recipe table with the mode's closed list (row i is mode recipe i).
int rc_mode_build_recipes(const RcModeRules *rules, RcSkillData *skills, RcModeLedger *ledger);

// ---- Queries -------------------------------------------------------------------------------------

int rc_mode_sale_value(const RcModeRules *rules, int item_id);   // -1 unknown, 0 worthless
int rc_mode_purchase_value(const RcModeRules *rules, int item_id);  // shop price or mode value
const RcModeNpc *rc_mode_npc(const RcModeRules *rules, int npc_id);
const RcModeDrop *rc_mode_drop(const RcModeRules *rules, int npc_id);
const RcModeBoneWeapon *rc_mode_bone_weapon(const RcModeRules *rules, int item_id);
bool rc_mode_skill_enabled(const RcModeRules *rules, int skill);
int rc_spell_element(const char *spell_name);

#endif
