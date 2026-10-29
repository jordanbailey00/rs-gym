#include "mode.h"

#include "assets.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const RcModeRules *g_active_rules;
static const RcModeLedger *g_active_ledger;

const RcModeRules *rc_mode_active(void) { return g_active_rules; }

void rc_mode_set_active(const RcModeRules *rules, const RcModeLedger *ledger) {
    g_active_rules = rules;
    g_active_ledger = rules ? ledger : NULL;
}
const RcModeLedger *rc_mode_active_ledger(void) { return g_active_rules ? g_active_ledger : NULL; }

// ---- Record parsing ------------------------------------------------------------------------------

#define MAX_FIELDS 48
#define VALUE_LEN 768

typedef struct {
    char key[32];
    char value[VALUE_LEN];
} Field;

typedef struct {
    char kind[24];
    Field fields[MAX_FIELDS];
    int count;
    int line;
    char *err;
    size_t err_cap;
    int failed;
} Record;

static void fail(Record *r, const char *fmt, ...) {
    if (r->failed) return;
    r->failed = 1;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (r->err && r->err_cap) snprintf(r->err, r->err_cap, "line %d (%s): %s", r->line, r->kind, msg);
}

static int split_record(char *line, Record *r) {
    char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (!*p || *p == '#') return 0;
    int n = 0;
    while (*p && !isspace((unsigned char)*p) && n < (int)sizeof(r->kind) - 1) r->kind[n++] = *p++;
    r->kind[n] = 0;
    r->count = 0;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (r->count >= MAX_FIELDS) {
            fail(r, "too many fields");
            return 1;
        }
        Field *f = &r->fields[r->count++];
        n = 0;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && n < (int)sizeof(f->key) - 1)
            f->key[n++] = *p++;
        f->key[n] = 0;
        if (*p != '=') {
            fail(r, "field %s has no value", f->key);
            return 1;
        }
        p++;
        n = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && n < VALUE_LEN - 1) f->value[n++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && !isspace((unsigned char)*p) && n < VALUE_LEN - 1) f->value[n++] = *p++;
        }
        f->value[n] = 0;
    }
    return 1;
}

static const char *get(Record *r, const char *key) {
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->fields[i].key, key) == 0) return r->fields[i].value;
    return NULL;
}

static int num(Record *r, const char *key, int fallback) {
    const char *v = get(r, key);
    if (!v) return fallback;
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v || *end) {
        fail(r, "%s=%s is not a number", key, v);
        return fallback;
    }
    return (int)x;
}

static int need(Record *r, const char *key) {
    if (!get(r, key)) {
        fail(r, "missing %s", key);
        return 0;
    }
    return num(r, key, 0);
}

static void text(Record *r, const char *key, char *out, size_t cap) {
    const char *v = get(r, key);
    snprintf(out, cap, "%s", v ? v : "");
}

// Parses "a:b:c,d:e:f" into rows of `width` ints; returns the row count, -1 on a malformed list.
static int tuples(Record *r, const char *key, int *out, int width, int max_rows) {
    const char *v = get(r, key);
    if (!v || !*v) return 0;
    int rows = 0, col = 0;
    const char *p = v;
    while (*p) {
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p) {
            fail(r, "%s: bad list near '%s'", key, p);
            return -1;
        }
        if (rows >= max_rows) {
            fail(r, "%s: more than %d entries", key, max_rows);
            return -1;
        }
        out[rows * width + col] = (int)x;
        p = end;
        if (*p == ':') {
            if (++col >= width) {
                fail(r, "%s: more than %d values in an entry", key, width);
                return -1;
            }
            p++;
        } else {
            if (col != width - 1) {
                fail(r, "%s: entry with %d of %d values", key, col + 1, width);
                return -1;
            }
            col = 0;
            rows++;
            if (*p == ',') p++;
            else if (*p) {
                fail(r, "%s: bad separator '%c'", key, *p);
                return -1;
            }
        }
    }
    return rows;
}

static int station_id(const char *name) {
    static const struct { const char *name; RcMakeMenu menu; } table[] = {
        {"furnace", RC_MAKE_MENU_FURNACE}, {"anvil", RC_MAKE_MENU_ANVIL},
        {"knife", RC_MAKE_MENU_KNIFE}, {"combine", RC_MAKE_MENU_COMBINE},
        {"spin", RC_MAKE_MENU_SPIN}, {"needle", RC_MAKE_MENU_NEEDLE},
        {"altar", RC_MAKE_MENU_ALTAR}, {"chisel", RC_MAKE_MENU_CHISEL},
        {"tanner", RC_MAKE_MENU_TANNER}, {"aldo", RC_MAKE_MENU_ALDO},
    };
    for (int i = 0; i < COUNT(table); i++)
        if (strcmp(table[i].name, name) == 0) return table[i].menu;
    return -1;
}

int rc_spell_element(const char *name) {
    if (!name) return RC_ELEMENT_NONE;
    if (!strncmp(name, "Wind ", 5)) return RC_ELEMENT_AIR;
    if (!strncmp(name, "Water ", 6)) return RC_ELEMENT_WATER;
    if (!strncmp(name, "Earth ", 6)) return RC_ELEMENT_EARTH;
    if (!strncmp(name, "Fire ", 5)) return RC_ELEMENT_FIRE;
    return RC_ELEMENT_NONE;
}

static int element_id(const char *name) {
    static const char *names[] = {"none", "air", "water", "earth", "fire"};
    for (int i = 0; i < COUNT(names); i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

static void parse_drop_rows(Record *r, const char *key, RcModeDropRow *rows, int *count, int width) {
    int v[RC_MODE_MAX_DROP_ROWS * 4];
    int n = tuples(r, key, v, width, RC_MODE_MAX_DROP_ROWS);
    for (int i = 0; i < n; i++)
        rows[i] = (RcModeDropRow){v[i * width], v[i * width + 1], v[i * width + 2],
                                  width > 3 ? v[i * width + 3] : 1};
    *count = n > 0 ? n : 0;
}

static RcModeDrop *drop_for(RcModeRules *m, Record *r, int npc) {
    for (int i = 0; i < m->drop_count; i++)
        if (m->drops[i].npc == npc) return &m->drops[i];
    if (m->drop_count >= RC_MODE_MAX_NPCS) {
        fail(r, "too many drop tables");
        return NULL;
    }
    RcModeDrop *d = &m->drops[m->drop_count++];
    memset(d, 0, sizeof(*d));
    d->npc = npc;
    return d;
}

#define ADD(arr, count, cap) \
    ((count) < (cap) ? &(arr)[(count)++] : (fail(r, "more than %d %s records", (cap), r->kind), NULL))

static void apply_record(RcModeRules *m, Record *r) {
    const char *k = r->kind;
    if (!strcmp(k, "mode")) {
        text(r, "name", m->name, sizeof(m->name));
        m->version = need(r, "version");
        m->level_cap = need(r, "cap");
        m->xp_multiplier = need(r, "xp_mult");
        m->max_players = need(r, "max_players");
        m->members = num(r, "members", 0) != 0;
        m->horizon = num(r, "horizon", 0);
        if (m->level_cap < 1 || m->level_cap > 99 || m->xp_multiplier < 1)
            fail(r, "cap must be 1..99 and xp_mult at least 1");
    } else if (!strcmp(k, "skills")) {
        m->skill_mask = (uint32_t)need(r, "mask");
    } else if (!strcmp(k, "death")) {
        m->respawn_ticks = need(r, "respawn");
        m->recovery_ticks = need(r, "recovery");
    } else if (!strcmp(k, "loot")) {
        m->private_ticks = need(r, "private");
        m->public_ticks = need(r, "public");
    } else if (!strcmp(k, "npcrespawn")) {
        m->npc_respawn = need(r, "ticks");
    } else if (!strcmp(k, "start")) {
        const char *where = get(r, "where");
        int kind = !where ? -1 : !strcmp(where, "equip") ? RC_MODE_START_EQUIP
                 : !strcmp(where, "inv") ? RC_MODE_START_INVENTORY
                 : !strcmp(where, "bank") ? RC_MODE_START_BANK : -1;
        if (kind < 0) {
            fail(r, "where must be equip, inv or bank");
            return;
        }
        RcModeStack *s = ADD(m->start[kind], m->start_count[kind], RC_MODE_MAX_START);
        if (s) *s = (RcModeStack){need(r, "item"), need(r, "qty")};
    } else if (!strcmp(k, "equip")) {
        RcModeEquip *e = ADD(m->equip, m->equip_count, RC_MODE_MAX_EQUIP);
        if (!e) return;
        memset(e, 0, sizeof(*e));
        e->item = need(r, "item");
        int v[SKILL_COUNT * 2];
        int n = tuples(r, "levels", v, 2, SKILL_COUNT);
        for (int i = 0; i < n; i++) {
            if (v[i * 2] < 0 || v[i * 2] >= SKILL_COUNT) fail(r, "bad skill %d", v[i * 2]);
            else e->level[v[i * 2]] = (uint8_t)v[i * 2 + 1];
        }
    } else if (!strcmp(k, "bonus")) {
        int item = need(r, "item");
        RcModeBoneWeapon *b = NULL;
        for (int i = 0; i < m->bone_count; i++) if (m->bone[i].item == item) b = &m->bone[i];
        if (!b && (b = ADD(m->bone, m->bone_count, RC_MODE_MAX_BONE))) {
            memset(b, 0, sizeof(*b));
            b->item = item;
        }
        if (b) b->base = need(r, "base");
    } else if (!strcmp(k, "boneweapon")) {
        int item = need(r, "item");
        RcModeBoneWeapon *b = NULL;
        for (int i = 0; i < m->bone_count; i++) if (m->bone[i].item == item) b = &m->bone[i];
        if (!b) {
            fail(r, "boneweapon %d without a bonus record", item);
            return;
        }
        b->rat_bonus = need(r, "rat_bonus");
        b->powered_max = num(r, "powered_max", 0);
        b->speed = num(r, "speed", 0);
        b->charge_item = num(r, "charge", 0);
        b->rat_count = tuples(r, "rats", b->rats, 1, RC_MODE_MAX_RATS);
    } else if (!strcmp(k, "tool")) {
        const char *kind = get(r, "kind");
        int axe = kind && !strcmp(kind, "axe");
        RcSkillTool *t = axe ? ADD(m->axes, m->axe_count, RC_MODE_MAX_TOOLS)
                             : ADD(m->pickaxes, m->pickaxe_count, RC_MODE_MAX_TOOLS);
        if (!t) return;
        *t = (RcSkillTool){.item_id = (uint32_t)need(r, "item"), .level = (uint8_t)need(r, "level"),
                           .ticks = (uint8_t)num(r, "ticks", 0), .scale = (uint16_t)num(r, "scale", 0)};
    } else if (!strcmp(k, "gather")) {
        RcGatherRule *g = ADD(m->gather, m->gather_count, RC_MODE_MAX_GATHER);
        if (!g) return;
        *g = (RcGatherRule){
            .obj_id = (uint32_t)need(r, "loc"), .product = (uint32_t)need(r, "product"),
            .skill = (uint8_t)need(r, "skill"), .level = (uint8_t)need(r, "level"),
            .xp_tenths = (uint16_t)need(r, "xp"), .low = (uint16_t)need(r, "low"),
            .high = (uint16_t)need(r, "high"), .deplete_one_in = (uint8_t)need(r, "deplete"),
            .level_offset = (int8_t)num(r, "offset", 0), .ticks = (uint8_t)num(r, "ticks", 0),
            .flags = (uint8_t)(num(r, "notool", 0) ? RC_GATHER_NO_TOOL : 0),
        };
    } else if (!strcmp(k, "fish")) {
        RcFishRule *f = ADD(m->fish, m->fish_count, RC_MODE_MAX_FISH);
        if (!f) return;
        memset(f, 0, sizeof(*f));
        f->npc_id = (uint32_t)need(r, "npc");
        f->option = (uint8_t)need(r, "option");
        f->tool = (uint32_t)need(r, "tool");
        f->bait = (uint32_t)num(r, "bait", 0);
        int v[RC_FISH_MAX_CATCHES * 6];
        int n = tuples(r, "catch", v, 6, RC_FISH_MAX_CATCHES);
        for (int i = 0; i < n; i++)
            f->catches[i] = (RcFishCatch){.product = (uint32_t)v[i * 6], .level = (uint8_t)v[i * 6 + 1],
                .level_offset = (int8_t)v[i * 6 + 2], .xp_tenths = (uint16_t)v[i * 6 + 3],
                .low = (uint16_t)v[i * 6 + 4], .high = (uint16_t)v[i * 6 + 5]};
        f->catch_count = (uint8_t)(n > 0 ? n : 0);
        if (n <= 0) fail(r, "a fishing option needs a catch");
    } else if (!strcmp(k, "cookobj")) {
        uint32_t *o = ADD(m->cook_objects, m->cook_object_count, RC_MODE_MAX_COOK_OBJECTS);
        if (o) *o = (uint32_t)need(r, "loc");
    } else if (!strcmp(k, "cook")) {
        RcCookRule *c = ADD(m->cook, m->cook_count, RC_MODE_MAX_COOK);
        if (!c) return;
        *c = (RcCookRule){.raw = (uint32_t)need(r, "raw"), .cooked = (uint32_t)need(r, "cooked"),
            .burnt = (uint32_t)need(r, "burnt"), .level = (uint8_t)need(r, "level"),
            .xp_tenths = (uint16_t)need(r, "xp"), .low = (uint16_t)need(r, "low"),
            .high = (uint16_t)need(r, "high"), .level_offset = (int8_t)num(r, "offset", 0)};
    } else if (!strcmp(k, "station")) {
        if (m->station_count >= RC_MODE_MAX_STATIONS) {
            fail(r, "too many stations");
            return;
        }
        int st = station_id(get(r, "kind") ? get(r, "kind") : "");
        if (st < 0) fail(r, "unknown station kind");
        m->stations[m->station_count].station = (uint8_t)st;
        m->stations[m->station_count++].loc = need(r, "loc");
    } else if (!strcmp(k, "recipe")) {
        int id = need(r, "id");
        if (id != m->recipe_count) {
            fail(r, "recipe ids must count up from 0 (got %d, expected %d)", id, m->recipe_count);
            return;
        }
        RcModeRecipe *x = ADD(m->recipes, m->recipe_count, RC_MODE_MAX_RECIPES);
        if (!x) return;
        memset(x, 0, sizeof(*x));
        x->id = id;
        int st = station_id(get(r, "station") ? get(r, "station") : "");
        if (st < 0) fail(r, "unknown station");
        x->station = (uint8_t)st;
        x->batch = num(r, "batch", 0) != 0;
        x->canonical = num(r, "canonical", -1);
        int locs[4];
        x->loc = tuples(r, "locs", locs, 1, 4) > 0 ? locs[0] : 0;
        RcRecipe *rec = &x->recipe;
        rec->output_item = (uint32_t)need(r, "out");
        rec->output_qty = (uint16_t)need(r, "qty");
        rec->output_state = (uint32_t)num(r, "state", 0);
        rec->ticks = (uint16_t)need(r, "ticks");
        int v[RC_RECIPE_MAX_INPUTS * 3];
        int n = tuples(r, "reqs", v, 3, RC_RECIPE_MAX_REQS);
        for (int i = 0; i < n; i++)
            rec->reqs[i] = (RcRecipeReq){(uint8_t)v[i * 3], (uint8_t)v[i * 3 + 1], (uint16_t)v[i * 3 + 2]};
        rec->req_count = (uint8_t)(n > 0 ? n : 0);
        n = tuples(r, "inputs", v, 2, RC_RECIPE_MAX_INPUTS);
        for (int i = 0; i < n; i++)
            rec->inputs[i] = (RcRecipeItemQty){(uint32_t)v[i * 2], (uint16_t)v[i * 2 + 1]};
        rec->input_count = (uint8_t)(n > 0 ? n : 0);
        n = tuples(r, "tools", v, 1, RC_RECIPE_MAX_TOOLS);
        for (int i = 0; i < n; i++) rec->tools[i] = (uint32_t)v[i];
        rec->tool_count = (uint8_t)(n > 0 ? n : 0);
        snprintf(rec->name, sizeof(rec->name), "mode recipe %d", id);
        snprintf(rec->facility, sizeof(rec->facility), "%s", get(r, "station") ? get(r, "station") : "");
    } else if (!strcmp(k, "spell")) {
        RcModeSpell *s = ADD(m->spells, m->spell_count, RC_MODE_MAX_SPELLS);
        if (!s) return;
        text(r, "name", s->name, sizeof(s->name));
        s->level = (uint8_t)need(r, "level");
        s->max_hit = (uint16_t)need(r, "max");
    } else if (!strcmp(k, "prayer")) {
        RcModePrayer *p = ADD(m->prayers, m->prayer_count, RC_MODE_MAX_PRAYERS);
        if (!p) return;
        text(r, "name", p->name, sizeof(p->name));
        p->level = (uint8_t)need(r, "level");
    } else if (!strcmp(k, "bury")) {
        RcModeBury *b = ADD(m->bury, m->bury_count, RC_MODE_MAX_BURY);
        if (b) *b = (RcModeBury){need(r, "item"), need(r, "xp")};
    } else if (!strcmp(k, "npc")) {
        RcModeNpc *n = ADD(m->npcs, m->npc_count, RC_MODE_MAX_NPCS);
        if (!n) return;
        memset(n, 0, sizeof(*n));
        n->id = need(r, "id");
        n->hp = need(r, "hp");
        n->max_hit = need(r, "max");
        int v[5];
        if (get(r, "defence")) {
            if (tuples(r, "defence", v, 1, 5) != 5) fail(r, "defence needs five bonuses");
            for (int i = 0; i < 5; i++) n->defence[i] = (int16_t)v[i];
            n->has_defence = true;
        }
        const char *w = get(r, "weakness");
        if (w) {
            char element[16] = {0};
            int pct = 0;
            if (sscanf(w, "%15[a-z]:%d", element, &pct) != 2 || element_id(element) < 0)
                fail(r, "weakness must be element:percent");
            n->weakness_element = (uint8_t)element_id(element);
            n->weakness_pct = (uint8_t)pct;
        }
    } else if (!strcmp(k, "drop")) {
        RcModeDrop *d = drop_for(m, r, need(r, "npc"));
        if (d) parse_drop_rows(r, "always", d->always, &d->always_count, 3);
    } else if (!strcmp(k, "droproll")) {
        RcModeDrop *d = drop_for(m, r, need(r, "npc"));
        if (!d) return;
        if (d->roll_count >= RC_MODE_MAX_DROP_ROLLS) {
            fail(r, "too many drop rolls");
            return;
        }
        RcModeDropRoll *roll = &d->rolls[d->roll_count++];
        roll->one_in = need(r, "one_in");
        parse_drop_rows(r, "pick", roll->pick, &roll->count, 4);
        if (roll->one_in < 1 || roll->count < 1) fail(r, "a drop roll needs one_in >= 1 and rows");
    } else if (!strcmp(k, "shop")) {
        int idx = need(r, "idx");
        if (idx != m->shop_count || idx >= RC_MODE_MAX_SHOPS) {
            fail(r, "shop indexes must count up from 0");
            return;
        }
        RcModeShop *s = &m->shops[m->shop_count++];
        memset(s, 0, sizeof(*s));
        text(r, "name", s->name, sizeof(s->name));
        s->npc = need(r, "npc");
        const char *buys = get(r, "buys");
        s->buys_valued = buys && !strcmp(buys, "valued");
    } else if (!strcmp(k, "stock")) {
        int shop = need(r, "shop");
        if (shop < 0 || shop >= m->shop_count) {
            fail(r, "stock for unknown shop %d", shop);
            return;
        }
        RcModeShop *s = &m->shops[shop];
        RcModeStock *st = ADD(s->stock, s->stock_count, RC_MODE_MAX_STOCK);
        if (st) *st = (RcModeStock){need(r, "item"), need(r, "price"), need(r, "cap"),
                                    need(r, "qty"), need(r, "ticks")};
    } else if (!strcmp(k, "sale")) {
        RcModeValue *v = ADD(m->sale, m->sale_count, RC_MODE_MAX_VALUES);
        if (v) *v = (RcModeValue){need(r, "item"), need(r, "value")};
    } else if (!strcmp(k, "purchase")) {
        RcModeValue *v = ADD(m->purchase, m->purchase_count, COUNT(m->purchase));
        if (v) *v = (RcModeValue){need(r, "item"), need(r, "value")};
    } else if (!strcmp(k, "service")) {
        const char *kind = get(r, "kind");
        if (kind && !strcmp(kind, "tanner")) m->tanner_npc = need(r, "npc");
        else if (kind && !strcmp(kind, "aldo")) m->aldo_npc = need(r, "npc");
        else fail(r, "unknown service");
    } else if (!strcmp(k, "boss")) {
        RcModeBoss *b = &m->boss;
        m->has_boss = true;
        b->npc = need(r, "npc");
        b->minion = need(r, "minion");
        b->hp = need(r, "hp");
        b->group_scale_pct = need(r, "group_scale_pct");
        b->max_hit = need(r, "max_hit");
        b->attack_speed = need(r, "attack_speed");
        b->final_attack_speed = need(r, "final_attack_speed");
        b->attack = need(r, "attack");
        b->defence = need(r, "defence");
        b->magic = need(r, "magic");
        int v[5];
        if (tuples(r, "defence_bonus", v, 1, 5) != 5) fail(r, "defence_bonus needs five values");
        for (int i = 0; i < 5; i++) b->defence_bonus[i] = (int16_t)v[i];
        b->hazard_max_hit = need(r, "hazard_max_hit");
        b->hazard_warning_ticks = need(r, "hazard_warning_ticks");
        b->hazard_interval = need(r, "hazard_interval");
        b->minion_hp = need(r, "minion_hp");
        b->minion_max_hit = need(r, "minion_max_hit");
        b->minion_attack_speed = need(r, "minion_attack_speed");
        b->minions_per_summon = need(r, "minions_per_summon");
        b->max_minions = need(r, "max_minions");
        b->summon_interval = need(r, "summon_interval");
        b->feed_heal = need(r, "feed_heal");
        b->feed_heals = need(r, "feed_heals");
        b->hazard_random_tiles = need(r, "hazard_random_tiles");
        b->feed_ticks = need(r, "feed_ticks");
        b->feed_interval = need(r, "feed_interval");
        b->phase2_pct = need(r, "phase2_pct");
        b->phase3_pct = need(r, "phase3_pct");
        b->respawn_ticks = need(r, "respawn_ticks");
        b->reset_ticks = need(r, "reset_ticks");
        b->qualify_pct = need(r, "qualify_pct");
    } else if (!strcmp(k, "arena")) {
        RcModeBoss *b = &m->boss;
        if (tuples(r, "rect", b->rect, 1, 4) != 4 || tuples(r, "centre", b->centre, 1, 2) != 2
                || tuples(r, "spawn", b->spawn, 1, 2) != 2)
            fail(r, "arena needs rect, centre and spawn");
        b->food_count = tuples(r, "food", &b->food[0][0], 2, RC_MODE_MAX_FOOD_PILES);
    } else if (!strcmp(k, "bossloot")) {
        RcModeBoss *b = &m->boss;
        int v[12];
        int n = tuples(r, "always", v, 3, 4);
        for (int i = 0; i < n; i++) b->always[i] = (RcModeDropRow){v[i * 3], v[i * 3 + 1], v[i * 3 + 2], 1};
        b->always_count = n > 0 ? n : 0;
        n = tuples(r, "bundle", v, 2, 4);
        for (int i = 0; i < n; i++) b->bundle[i] = (RcModeStack){v[i * 2], v[i * 2 + 1]};
        b->bundle_count = n > 0 ? n : 0;
        n = tuples(r, "extra", v, 3, 4);
        for (int i = 0; i < n; i++) {
            b->extra[i].item = v[i * 3];
            b->extra[i].qty = v[i * 3 + 1];
            b->extra[i].one_in = v[i * 3 + 2];
        }
        b->extra_count = n > 0 ? n : 0;
    } else {
        fail(r, "unknown record");
    }
}

int rc_mode_rules_load(const char *path, RcModeRules *out, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));
    FILE *f = rc_asset_fopen(path, "r");
    if (!f) {
        if (err) snprintf(err, err_cap, "cannot open %s", path);
        return 0;
    }
    static char line[16384];
    Record *r = calloc(1, sizeof(*r));
    if (!r) {
        rc_asset_close(f);
        return 0;
    }
    r->err = err;
    r->err_cap = err_cap;
    int ok = 1;
    while (fgets(line, sizeof(line), f)) {
        r->line++;
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            snprintf(err, err_cap, "line %d is too long", r->line);
            ok = 0;
            break;
        }
        if (!split_record(line, r)) continue;
        if (!r->failed) apply_record(out, r);
        if (r->failed) {
            ok = 0;
            break;
        }
    }
    rc_asset_close(f);
    free(r);
    if (ok && (!out->name[0] || !out->level_cap)) {
        if (err) snprintf(err, err_cap, "%s has no mode record", path);
        ok = 0;
    }
    return ok;
}

// ---- Ledger ---------------------------------------------------------------------------------------

void rc_mode_ledger_free(RcModeLedger *ledger) {
    if (!ledger) return;
    free(ledger->rows);
    memset(ledger, 0, sizeof(*ledger));
}

static void log_change(RcModeLedger *ledger, const char *kind, int id, const char *name,
                       const char *field, const char *from, const char *to) {
    if (!ledger || !strcmp(from, to)) return;
    if (ledger->count == ledger->cap) {
        int cap = ledger->cap ? ledger->cap * 2 : 256;
        RcModeOverride *rows = realloc(ledger->rows, (size_t)cap * sizeof(*rows));
        if (!rows) return;
        ledger->rows = rows;
        ledger->cap = cap;
    }
    RcModeOverride *o = &ledger->rows[ledger->count++];
    snprintf(o->kind, sizeof(o->kind), "%s", kind);
    o->id = id;
    snprintf(o->name, sizeof(o->name), "%s", name ? name : "");
    snprintf(o->field, sizeof(o->field), "%s", field);
    snprintf(o->from, sizeof(o->from), "%s", from);
    snprintf(o->to, sizeof(o->to), "%s", to);
}

static void log_int(RcModeLedger *ledger, const char *kind, int id, const char *name,
                    const char *field, int from, int to) {
    char a[24], b[24];
    snprintf(a, sizeof(a), "%d", from);
    snprintf(b, sizeof(b), "%d", to);
    log_change(ledger, kind, id, name, field, a, b);
}

static const char *const skill_names[SKILL_COUNT] = {
    "Attack", "Defence", "Strength", "Hitpoints", "Ranged", "Prayer", "Magic", "Cooking",
    "Woodcutting", "Fletching", "Fishing", "Firemaking", "Crafting", "Smithing", "Mining",
    "Herblore", "Agility", "Thieving", "Slayer", "Farming", "Runecraft", "Hunter", "Construction",
};

static void describe_reqs(const RcItemDef *d, char *out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < d->req_count && used < cap; i++) {
        int s = d->req_skill[i];
        used += (size_t)snprintf(out + used, cap - used, "%s%s %d", i ? ", " : "",
                                 s >= 0 && s < SKILL_COUNT ? skill_names[s] : "?", d->req_level[i]);
    }
    if (!out[0]) snprintf(out, cap, "none");
}

// ---- Definition patches ---------------------------------------------------------------------------

int rc_mode_patch_items(const RcModeRules *m, RcItemDef *defs, int count, RcModeLedger *ledger) {
    if (!m || !defs) return 0;
    for (int i = 0; i < m->equip_count; i++) {
        const RcModeEquip *e = &m->equip[i];
        if (e->item < 0 || e->item >= count || !defs[e->item].loaded) return 0;
        RcItemDef *d = &defs[e->item];
        char from[96], to[96];
        describe_reqs(d, from, sizeof(from));
        d->req_count = 0;
        for (int s = 0; s < SKILL_COUNT; s++) {
            if (!e->level[s]) continue;
            d->req_skill[d->req_count] = s;
            d->req_level[d->req_count++] = e->level[s];
        }
        describe_reqs(d, to, sizeof(to));
        log_change(ledger, "item", d->id, d->name, "use requirements", from, to);
    }
    for (int i = 0; i < m->bone_count; i++) {
        const RcModeBoneWeapon *b = &m->bone[i];
        if (b->item < 0 || b->item >= count || b->base < 0 || b->base >= count
                || !defs[b->item].loaded || !defs[b->base].loaded)
            return 0;
        RcItemDef *d = &defs[b->item];
        const RcItemDef *s = &defs[b->base];
        char from[96], to[96];
        snprintf(from, sizeof(from), "att %d/%d/%d/%d/%d str %d rstr %d mdmg %d",
                 d->attack_stab, d->attack_slash, d->attack_crush, d->attack_magic, d->attack_ranged,
                 d->strength_bonus, d->ranged_strength, d->magic_damage);
        d->attack_stab = s->attack_stab;
        d->attack_slash = s->attack_slash;
        d->attack_crush = s->attack_crush;
        d->attack_magic = s->attack_magic;
        d->attack_ranged = s->attack_ranged;
        d->defence_stab = s->defence_stab;
        d->defence_slash = s->defence_slash;
        d->defence_crush = s->defence_crush;
        d->defence_magic = s->defence_magic;
        d->defence_ranged = s->defence_ranged;
        d->strength_bonus = s->strength_bonus;
        d->ranged_strength = s->ranged_strength;
        d->magic_damage = s->magic_damage;
        d->prayer_bonus = s->prayer_bonus;
        snprintf(to, sizeof(to), "%.20s's: att %d/%d/%d/%d/%d str %d rstr %d mdmg %d", s->name,
                 d->attack_stab, d->attack_slash, d->attack_crush, d->attack_magic, d->attack_ranged,
                 d->strength_bonus, d->ranged_strength, d->magic_damage);
        log_change(ledger, "item", d->id, d->name, "combat bonuses", from, to);
        if (b->powered_max) {
            log_int(ledger, "item", d->id, d->name, "powered spell base max hit", 0, b->powered_max);
            if (b->speed && d->attack_speed != b->speed) {
                log_int(ledger, "item", d->id, d->name, "attack speed", d->attack_speed, b->speed);
                d->attack_speed = b->speed;
            }
        }
        log_int(ledger, "item", d->id, d->name, "max hit against rats", 0, b->rat_bonus);
    }
    return 1;
}

static RcNpcDef *npc_def(RcNpcDef *defs, int count, const int *by_id, int by_id_count, int id) {
    if (id < 0 || id >= by_id_count) return NULL;
    int idx = by_id[id];
    return idx >= 0 && idx < count ? &defs[idx] : NULL;
}

static void patch_npc(RcNpcDef *d, int hp, int max_hit, const int16_t *defence, RcModeLedger *ledger) {
    log_int(ledger, "npc", d->id, d->name, "hitpoints", d->hitpoints, hp);
    d->hitpoints = hp;
    d->stats[3] = hp;
    log_int(ledger, "npc", d->id, d->name, "max hit", d->max_hit, max_hit);
    d->max_hit = max_hit;
    if (defence) {
        char from[64], to[64];
        snprintf(from, sizeof(from), "%d/%d/%d/%d/%d", d->defence_bonus[0], d->defence_bonus[1],
                 d->defence_bonus[2], d->defence_bonus[3], d->defence_bonus[4]);
        for (int i = 0; i < 5; i++) d->defence_bonus[i] = defence[i];
        snprintf(to, sizeof(to), "%d/%d/%d/%d/%d", d->defence_bonus[0], d->defence_bonus[1],
                 d->defence_bonus[2], d->defence_bonus[3], d->defence_bonus[4]);
        log_change(ledger, "npc", d->id, d->name, "defence bonuses (stab/slash/crush/ranged/magic)",
                   from, to);
    }
}

int rc_mode_patch_npcs(const RcModeRules *m, RcNpcDef *defs, int count, const int *by_id,
                       int by_id_count, RcModeLedger *ledger) {
    if (!m || !defs || !by_id) return 0;
    static const char *elements[] = {"none", "air", "water", "earth", "fire"};
    for (int i = 0; i < m->npc_count; i++) {
        const RcModeNpc *n = &m->npcs[i];
        RcNpcDef *d = npc_def(defs, count, by_id, by_id_count, n->id);
        if (!d) return 0;
        patch_npc(d, n->hp, n->max_hit, n->has_defence ? n->defence : NULL, ledger);
        if (n->weakness_element) {
            char to[32];
            snprintf(to, sizeof(to), "%s %d%%", elements[n->weakness_element], n->weakness_pct);
            log_change(ledger, "npc", d->id, d->name, "elemental weakness", "none", to);
            d->weakness_element = n->weakness_element;
            d->weakness_pct = n->weakness_pct;
        }
        if (m->npc_respawn > 0) {
            log_int(ledger, "npc", d->id, d->name, "respawn ticks", d->respawn_ticks, m->npc_respawn);
            d->respawn_ticks = m->npc_respawn;
        }
    }
    if (m->has_boss) {
        const RcModeBoss *b = &m->boss;
        RcNpcDef *d = npc_def(defs, count, by_id, by_id_count, b->npc);
        RcNpcDef *rat = npc_def(defs, count, by_id, by_id_count, b->minion);
        if (!d || !rat) return 0;
        patch_npc(d, b->hp, b->max_hit, b->defence_bonus, ledger);
        int levels[6] = {b->attack, b->defence, b->attack, b->hp, b->attack, b->magic};
        static const char *stat_names[6] = {"attack", "defence", "strength", "hitpoints", "ranged",
                                            "magic"};
        for (int s = 0; s < 6; s++) {
            if (s == 3) continue;
            char field[24];
            snprintf(field, sizeof(field), "%s level", stat_names[s]);
            log_int(ledger, "npc", d->id, d->name, field, d->stats[s], levels[s]);
            d->stats[s] = levels[s];
        }
        log_int(ledger, "npc", d->id, d->name, "attack speed", d->attack_speed, b->attack_speed);
        d->attack_speed = b->attack_speed;
        log_int(ledger, "npc", d->id, d->name, "respawn ticks", d->respawn_ticks, b->respawn_ticks);
        d->respawn_ticks = b->respawn_ticks;
        patch_npc(rat, b->minion_hp, b->minion_max_hit, NULL, ledger);
        log_int(ledger, "npc", rat->id, rat->name, "attack speed", rat->attack_speed,
                b->minion_attack_speed);
        rat->attack_speed = b->minion_attack_speed;
        rat->respawn_ticks = 0;
    }
    return 1;
}

static const RcModeSpell *mode_spell(const RcModeRules *m, const char *name) {
    for (int i = 0; i < m->spell_count; i++)
        if (!strcmp(m->spells[i].name, name)) return &m->spells[i];
    return NULL;
}

int rc_mode_patch_spells(const RcModeRules *m, RcSpellDef *defs, int count, RcModeLedger *ledger) {
    if (!m || !defs) return 0;
    int found = 0, disabled = 0;
    for (int i = 0; i < count; i++) {
        RcSpellDef *d = &defs[i];
        if (!d->loaded) continue;
        const RcModeSpell *s = mode_spell(m, d->name);
        if (!s) {
            if (d->level <= m->level_cap) disabled++;
            d->level = 255;  // outside the mode's whitelist
            continue;
        }
        found++;
        log_int(ledger, "spell", i, d->name, "level", d->level, s->level);
        d->level = s->level;
        log_int(ledger, "spell", i, d->name, "base max hit", d->max_hit, s->max_hit);
        d->max_hit = s->max_hit;
    }
    if (found != m->spell_count) return 0;
    char to[48];
    snprintf(to, sizeof(to), "%d spells unavailable", disabled);
    log_change(ledger, "spell", -1, "every spell outside the whitelist", "availability",
               "canonical", to);
    return 1;
}

int rc_mode_patch_prayers(const RcModeRules *m, RcPrayerDef *defs, int count, RcModeLedger *ledger) {
    if (!m || !defs) return 0;
    int found = 0, disabled = 0;
    for (int i = 0; i < count; i++) {
        RcPrayerDef *d = &defs[i];
        if (!d->loaded) continue;
        const RcModePrayer *p = NULL;
        for (int j = 0; j < m->prayer_count; j++)
            if (!strcmp(m->prayers[j].name, d->name)) p = &m->prayers[j];
        if (!p) {
            disabled++;
            d->level = 255;
            continue;
        }
        found++;
        log_int(ledger, "prayer", d->id, d->name, "level", d->level, p->level);
        d->level = p->level;
        if (d->unlock_varbit) {
            log_change(ledger, "prayer", d->id, d->name, "unlock", "varbit", "none");
            d->unlock_varbit = 0;
            d->unlock_value = 0;
        }
    }
    if (found != m->prayer_count) return 0;
    char to[48];
    snprintf(to, sizeof(to), "%d prayers unavailable", disabled);
    log_change(ledger, "prayer", -1, "every prayer outside the whitelist", "availability",
               "canonical", to);
    return 1;
}

static void describe_recipe(const RcRecipe *r, char *out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < r->req_count && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%s%s %d", i ? "+" : "",
                                 r->reqs[i].skill < SKILL_COUNT ? skill_names[r->reqs[i].skill] : "?",
                                 r->reqs[i].level);
    for (int i = 0; i < r->input_count && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%s%ux%u", i || r->req_count ? " " : "",
                                 r->inputs[i].item_id, r->inputs[i].quantity);
    for (int i = 0; i < r->tool_count && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, " tool %u", r->tools[i]);
}

int rc_mode_build_recipes(const RcModeRules *m, RcSkillData *skills, RcModeLedger *ledger) {
    if (!m || !skills) return 0;
    int n = m->recipe_count;
    RcRecipe *rows = calloc((size_t)(n ? n : 1), sizeof(*rows));
    int *next = calloc((size_t)(n ? n : 1), sizeof(*next));
    if (!rows || !next) {
        free(rows);
        free(next);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        const RcModeRecipe *x = &m->recipes[i];
        rows[i] = x->recipe;
        const RcRecipe *canon = x->canonical >= 0 && x->canonical < skills->recipe_count
                              ? &skills->recipes[x->canonical] : NULL;
        if (x->canonical >= 0 && (!canon || canon->output_item != x->recipe.output_item)) {
            free(rows);
            free(next);
            return 0;
        }
        const RcItemDef *out = rc_item_def_get((int)rows[i].output_item);
        if (canon) snprintf(rows[i].name, sizeof(rows[i].name), "%s", canon->name);
        else if (out) snprintf(rows[i].name, sizeof(rows[i].name), "%s", out->name);
        char from[96], to[96];
        if (canon) describe_recipe(canon, from, sizeof(from));
        else snprintf(from, sizeof(from), "no canonical row");
        describe_recipe(&rows[i], to, sizeof(to));
        log_change(ledger, "recipe", i, rows[i].name, canon ? "requirements and inputs" : "added",
                   from, to);
    }
    for (int i = 0; i < RC_MAX_ITEM_DEFS; i++) skills->recipe_by_output[i] = -1;
    for (int i = n - 1; i >= 0; i--) {
        uint32_t out = rows[i].output_item;
        next[i] = out < RC_MAX_ITEM_DEFS ? skills->recipe_by_output[out] : -1;
        if (out < RC_MAX_ITEM_DEFS) skills->recipe_by_output[out] = i;
    }
    char from[32], to[32];
    snprintf(from, sizeof(from), "%d rows", skills->recipe_count);
    snprintf(to, sizeof(to), "%d mode rows", n);
    log_change(ledger, "recipe", -1, "recipe table", "closed whitelist", from, to);
    free(skills->recipes);
    free(skills->recipe_next);
    skills->recipes = rows;
    skills->recipe_next = next;
    skills->recipe_count = n;
    return 1;
}

// ---- Queries --------------------------------------------------------------------------------------

int rc_mode_sale_value(const RcModeRules *m, int item_id) {
    for (int i = 0; m && i < m->sale_count; i++)
        if (m->sale[i].item == item_id) return m->sale[i].value;
    return -1;
}

int rc_mode_purchase_value(const RcModeRules *m, int item_id) {
    for (int s = 0; m && s < m->shop_count; s++)
        for (int i = 0; i < m->shops[s].stock_count; i++)
            if (m->shops[s].stock[i].item == item_id) return m->shops[s].stock[i].price;
    for (int i = 0; m && i < m->purchase_count; i++)
        if (m->purchase[i].item == item_id) return m->purchase[i].value;
    return -1;
}

const RcModeNpc *rc_mode_npc(const RcModeRules *m, int npc_id) {
    for (int i = 0; m && i < m->npc_count; i++)
        if (m->npcs[i].id == npc_id) return &m->npcs[i];
    return NULL;
}

const RcModeDrop *rc_mode_drop(const RcModeRules *m, int npc_id) {
    for (int i = 0; m && i < m->drop_count; i++)
        if (m->drops[i].npc == npc_id) return &m->drops[i];
    return NULL;
}

const RcModeBoneWeapon *rc_mode_bone_weapon(const RcModeRules *m, int item_id) {
    for (int i = 0; m && i < m->bone_count; i++)
        if (m->bone[i].item == item_id) return &m->bone[i];
    return NULL;
}

bool rc_mode_skill_enabled(const RcModeRules *m, int skill) {
    return !m || (skill >= 0 && skill < 32 && (m->skill_mask & (1u << skill)));
}
