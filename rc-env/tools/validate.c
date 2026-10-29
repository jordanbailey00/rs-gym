// rs-gym-validate [mode]: loads a mode the way rc-env does and checks that its content is usable:
// every referenced id exists, use requirements stay within the level cap, no reachable item is a
// quest item, every mode arrow has a bow at or below its band, every station, node, enemy and
// service is placed in the map, and every shop good and drop has a sale value. Writes the override
// ledger (modes/<mode>/override-ledger.json) and a report (build/reports/<mode>-validation.txt);
// exits 1 when a check fails. tools/build_mode.py runs it after building a mode with rules.
#include "../../rc-content/combat/ammunition.h"
#include "../../rc-content/content.h"
#include "../../rc-core/api.h"
#include "../../rc-core/coordinates.h"
#include "../../rc-core/game_data.h"
#include "../../rc-core/items.h"
#include "../../rc-core/layout.h"
#include "../../rc-core/mode.h"
#include "../../rc-core/npc.h"
#include "../../rc-core/objects.h"
#include "../../rc-core/prayer.h"
#include "../../rc-core/skills.h"
#include "../../rc-core/spells.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FILE *report;
static int failures, checks;

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(report, fmt, ap);
    va_end(ap);
    fputc('\n', report);
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(report, "FAIL: "); \
    note(__VA_ARGS__); fprintf(stderr, "rs-gym-validate: "); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static const char *item_name(int id) {
    const RcItemDef *d = rc_item_def_get(id);
    return d && d->name[0] ? d->name : "?";
}

// ---- Reachable items ------------------------------------------------------------------------------

#define MAX_REACH 512
static int reach[MAX_REACH], reach_count;
static const char *reach_why[MAX_REACH];

static void reachable(int item, const char *why) {
    for (int i = 0; i < reach_count; i++) if (reach[i] == item) return;
    if (reach_count < MAX_REACH) {
        reach_why[reach_count] = why;
        reach[reach_count++] = item;
    }
}

static int is_reachable(int item) {
    for (int i = 0; i < reach_count; i++) if (reach[i] == item) return 1;
    return 0;
}

static void collect_reachable(const RcModeRules *m) {
    for (int k = 0; k < RC_MODE_START_KINDS; k++)
        for (int i = 0; i < m->start_count[k]; i++) reachable(m->start[k][i].item, "starter kit");
    for (int i = 0; i < m->gather_count; i++) reachable((int)m->gather[i].product, "gathering");
    for (int i = 0; i < m->fish_count; i++)
        for (int c = 0; c < m->fish[i].catch_count; c++)
            reachable((int)m->fish[i].catches[c].product, "fishing");
    for (int i = 0; i < m->cook_count; i++) {
        reachable((int)m->cook[i].cooked, "cooking");
        reachable((int)m->cook[i].burnt, "cooking");
    }
    for (int i = 0; i < m->recipe_count; i++)
        reachable((int)m->recipes[i].recipe.output_item, "production");
    for (int d = 0; d < m->drop_count; d++) {
        for (int i = 0; i < m->drops[d].always_count; i++) reachable(m->drops[d].always[i].item, "drop");
        for (int r = 0; r < m->drops[d].roll_count; r++)
            for (int i = 0; i < m->drops[d].rolls[r].count; i++)
                reachable(m->drops[d].rolls[r].pick[i].item, "drop");
    }
    for (int s = 0; s < m->shop_count; s++)
        for (int i = 0; i < m->shops[s].stock_count; i++) reachable(m->shops[s].stock[i].item, "shop");
    if (m->has_boss) {
        for (int i = 0; i < m->boss.always_count; i++) reachable(m->boss.always[i].item, "Scurrius");
        for (int i = 0; i < m->boss.bundle_count; i++) reachable(m->boss.bundle[i].item, "Scurrius");
        for (int i = 0; i < m->boss.extra_count; i++) reachable(m->boss.extra[i].item, "Scurrius");
    }
}

// ---- Checks ---------------------------------------------------------------------------------------

static void check_items(const RcModeRules *m) {
    for (int i = 0; i < reach_count; i++) {
        const RcItemDef *d = rc_item_def_get(reach[i]);
        CHECK(d && d->loaded, "item %d (%s) is not in the item definitions", reach[i], reach_why[i]);
        if (!d) continue;
        CHECK(!d->quest_item, "%s (%d) is a quest item", d->name, d->id);
        for (int r = 0; r < d->req_count; r++)
            CHECK(d->req_level[r] <= m->level_cap, "%s needs level %d in skill %d, above the cap %d",
                  d->name, d->req_level[r], d->req_skill[r], m->level_cap);
        if (reach[i] != 995)
            CHECK(rc_mode_sale_value(m, reach[i]) >= 0, "%s (%d) has no sale value", d->name, d->id);
    }
    for (int i = 0; i < m->recipe_count; i++) {
        const RcRecipe *r = rc_recipe_get(i);
        CHECK(r && r->output_item == m->recipes[i].recipe.output_item, "recipe %d is not row %d", i, i);
        for (int k = 0; r && k < r->input_count; k++)
            CHECK(is_reachable((int)r->inputs[k].item_id) || r->inputs[k].item_id == 995,
                  "recipe %d (%s) needs %s, which nothing supplies", i, item_name((int)r->output_item),
                  item_name((int)r->inputs[k].item_id));
        for (int k = 0; r && k < r->tool_count; k++)
            CHECK(is_reachable((int)r->tools[k]), "recipe %d (%s) needs the tool %s, which nothing "
                  "supplies", i, item_name((int)r->output_item), item_name((int)r->tools[k]));
        for (int k = 0; r && k < r->req_count; k++)
            CHECK(r->reqs[k].level <= m->level_cap, "recipe %d (%s) needs level %d", i,
                  item_name((int)r->output_item), r->reqs[k].level);
    }
    note("items: %d reachable, %d recipes", reach_count, m->recipe_count);
}

static void check_magic_prayer(const RcModeRules *m) {
    int count = 0;
    for (int i = 0; i < RC_MAX_SPELL_DEFS; i++) {
        const RcSpellDef *s = rc_spell_def_get(i);
        if (!s || !s->loaded || s->level > m->level_cap) continue;
        count++;
        for (int r = 0; r < s->rune_count; r++)
            CHECK(is_reachable((int)s->runes[r].item_id), "%s needs %s, which nothing supplies",
                  s->name, item_name((int)s->runes[r].item_id));
    }
    CHECK(count == m->spell_count, "%d spells are castable below the cap, expected %d", count,
          m->spell_count);
    int prayers = 0;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *p = rc_prayer_def_get(i);
        if (!p || !p->loaded || p->level > m->level_cap) continue;
        prayers++;
        CHECK(!p->unlock_varbit, "%s still needs an unlock", p->name);
    }
    CHECK(prayers == m->prayer_count, "%d prayers are usable below the cap, expected %d", prayers,
          m->prayer_count);
    note("magic and prayer: %d spells, %d prayers", count, prayers);
}

// The lowest-band mode bow that fires `arrow` (a use requirement in Ranged).
static int bow_band_for(const RcModeRules *m, int arrow, int *bow_out) {
    int best = 999;
    for (int i = 0; i < m->equip_count; i++) {
        const RcItemDef *bow = rc_item_def_get(m->equip[i].item);
        if (!bow || bow->equip_slot != EQUIP_WEAPON || !m->equip[i].level[SKILL_RANGED]) continue;
        RcPlayer p;
        memset(&p, 0, sizeof(p));
        for (int s = 0; s < RC_EQUIP_COUNT; s++) p.equipment[s].item_id = -1;
        p.equipment[EQUIP_WEAPON] = (RcInvSlot){.item_id = bow->id, .quantity = 1};
        p.equipment[EQUIP_AMMO] = (RcInvSlot){.item_id = arrow, .quantity = 1};
        const char *why = NULL;
        if (rc_content_ranged_resource_slot(&p, &why) == EQUIP_AMMO
                && m->equip[i].level[SKILL_RANGED] < best) {
            best = m->equip[i].level[SKILL_RANGED];
            *bow_out = bow->id;
        }
    }
    return best;
}

static void check_ammunition(const RcModeRules *m) {
    int arrows = 0;
    for (int i = 0; i < m->equip_count; i++) {
        const RcItemDef *d = rc_item_def_get(m->equip[i].item);
        if (!d || d->equip_slot != EQUIP_AMMO) continue;
        arrows++;
        int bow = -1, band = bow_band_for(m, d->id, &bow);
        CHECK(band <= m->equip[i].level[SKILL_RANGED], "%s (band %d) has no mode bow at or below "
              "its band", d->name, m->equip[i].level[SKILL_RANGED]);
        if (bow >= 0) note("ammunition: %s fires from %s (band %d)", d->name, item_name(bow), band);
    }
    CHECK(arrows > 0, "the mode has no arrows");
}

static int placed(const RcGymLayout *layout, int loc) {
    int n = 0;
    for (int rx = layout->min_rx; rx <= layout->max_rx; rx++) {
        for (int ry = layout->min_ry; ry <= layout->max_ry; ry++) {
            uint16_t key;
            if (!rc_world_to_mapsquare(rx * 64, ry * 64, &key, NULL, NULL)) continue;
            int count = 0;
            const RcObjectPlacement *rows = rc_object_region_placements(key, &count);
            for (int i = 0; rows && i < count; i++) n += (int)rows[i].obj_id == loc;
        }
    }
    return n;
}

static int nodes_of(const RcGymLayout *layout, int loc) {
    int n = 0;
    for (int rx = layout->min_rx; rx <= layout->max_rx; rx++) {
        for (int ry = layout->min_ry; ry <= layout->max_ry; ry++) {
            uint16_t key;
            if (!rc_world_to_mapsquare(rx * 64, ry * 64, &key, NULL, NULL)) continue;
            int count = 0;
            const RcGatheringNode *rows = rc_gathering_nodes_in_region(key, &count);
            for (int i = 0; rows && i < count; i++) n += (int)rows[i].obj_id == loc;
        }
    }
    return n;
}

static int spawned(const RcWorld *w, int npc_id) {
    int n = 0, count = 0;
    const RcNpc *npcs = rc_get_npcs(w, &count);
    for (int i = 0; i < count; i++) {
        const RcNpcDef *d = npcs[i].active ? rc_npc_def_for_npc(w, &npcs[i]) : NULL;
        n += d && d->id == npc_id;
    }
    return n;
}

static void check_world(const RcModeRules *m, const RcGymLayout *layout, const RcWorld *w) {
    for (int i = 0; i < m->station_count; i++)
        if (placed(layout, m->stations[i].loc))
            note("station %d: loc %d placed %d", m->stations[i].station, m->stations[i].loc,
                 placed(layout, m->stations[i].loc));
    for (int i = 0; i < m->recipe_count; i++) {
        if (m->recipes[i].station != RC_MAKE_MENU_ALTAR) continue;
        CHECK(placed(layout, m->recipes[i].loc) > 0, "the altar for %s (loc %d) is not placed",
              item_name((int)m->recipes[i].recipe.output_item), m->recipes[i].loc);
    }
    int cook = 0;
    for (int i = 0; i < m->cook_object_count; i++) cook += placed(layout, (int)m->cook_objects[i]);
    CHECK(cook > 0, "no range is placed");
    int seen[RC_MODE_MAX_GATHER] = {0};
    for (int i = 0; i < m->gather_count; i++) seen[i] = nodes_of(layout, (int)m->gather[i].obj_id);
    for (int i = 0; i < m->gather_count; i++) {
        int product = (int)m->gather[i].product, total = 0;
        for (int j = 0; j < m->gather_count; j++)
            if ((int)m->gather[j].product == product) total += seen[j];
        CHECK(total > 0, "no gathering node yields %s", item_name(product));
    }
    for (int i = 0; i < m->npc_count; i++)
        CHECK(spawned(w, m->npcs[i].id) > 0, "enemy %d is not spawned", m->npcs[i].id);
    for (int i = 0; i < m->fish_count; i++)
        CHECK(spawned(w, (int)m->fish[i].npc_id) > 0, "fishing spot %u is not spawned",
              m->fish[i].npc_id);
    for (int i = 0; i < m->shop_count; i++)
        CHECK(spawned(w, m->shops[i].npc) > 0, "shopkeeper %d is not spawned", m->shops[i].npc);
    if (m->tanner_npc) CHECK(spawned(w, m->tanner_npc) > 0, "the tanner is not spawned");
    if (m->aldo_npc) CHECK(spawned(w, m->aldo_npc) > 0, "Historian Aldo is not spawned");
    if (m->has_boss) CHECK(spawned(w, m->boss.npc) == 1, "Scurrius is spawned %d times",
                           spawned(w, m->boss.npc));
    int count = 0;
    const RcNpc *npcs = rc_get_npcs(w, &count);
    int active = 0;
    for (int i = 0; i < count; i++) active += npcs[i].active;
    note("world: %d NPCs spawned", active);
}

// ---- Ledger ---------------------------------------------------------------------------------------

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

static const char *reason(const RcModeOverride *o) {
    if (!strcmp(o->kind, "item") && !strcmp(o->field, "use requirements"))
        return "band rebase: bronze/steel/mithril, shortbow/oak/willow and leather/hardleather/"
               "studded tiers at 1/15/30 (SPEC.md, Progression bands)";
    if (!strcmp(o->kind, "item"))
        return "bone weapon improvement loop (SPEC.md, Rewards and equipment improvement)";
    if (!strcmp(o->kind, "npc") && strstr(o->name, "Scurrius"))
        return "Scurrius mode tuning (SPEC.md, Initial mode tuning)";
    if (!strcmp(o->kind, "npc") && !strcmp(o->field, "respawn ticks"))
        return "ordinary enemy respawn default (SPEC.md, World layout)";
    if (!strcmp(o->kind, "npc"))
        return "ordinary encounter profile (SPEC.md, Ordinary encounters and drop tables)";
    if (!strcmp(o->kind, "spell"))
        return "spell whitelist with within-tier elemental scaling (SPEC.md, Progression bands)";
    if (!strcmp(o->kind, "prayer"))
        return "prayer selection by band (SPEC.md, Prayer selection)";
    if (!strcmp(o->kind, "recipe") && !strcmp(o->field, "added"))
        return "service or recipe the data snapshot lacks (manifest recipe note)";
    if (!strcmp(o->kind, "recipe"))
        return "closed production chains rebased to the bands (SPEC.md, Closed production chains)";
    return "mode rule";
}

static void add_row(RcModeLedger *l, const char *kind, int id, const char *name,
                    const char *field, const char *from, const char *to) {
    if (l->count >= l->cap) return;
    RcModeOverride *o = &l->rows[l->count++];
    snprintf(o->kind, sizeof(o->kind), "%s", kind);
    o->id = id;
    snprintf(o->name, sizeof(o->name), "%s", name);
    snprintf(o->field, sizeof(o->field), "%s", field);
    snprintf(o->from, sizeof(o->from), "%s", from);
    snprintf(o->to, sizeof(o->to), "%s", to);
}

static void write_ledger(const char *path, const RcModeRules *m, const RcModeLedger *ledger) {
    FILE *f = fopen(path, "w");
    if (!f) {
        CHECK(0, "cannot write %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "{\n \"format\": \"rs-gym-override-ledger-v1\",\n \"mode\": ");
    json_string(f, m->name);
    fprintf(f, ",\n \"version\": %d,\n \"rules\": [\n", m->version);
    // Mode-wide rules that are not definition fields.
    char buf[96];
    struct { const char *rule, *from; char to[96]; const char *why; } rules[8];
    int n = 0;
    rules[n] = (typeof(rules[0])){"level cap", "99", "", "SPEC.md, Initial numerical defaults"};
    snprintf(rules[n++].to, sizeof(buf), "%d", m->level_cap);
    rules[n] = (typeof(rules[0])){"XP multiplier (enabled skills)", "1", "",
                                  "SPEC.md, Initial numerical defaults"};
    snprintf(rules[n++].to, sizeof(buf), "%d", m->xp_multiplier);
    rules[n] = (typeof(rules[0])){"death", "keep 3 items, gravestone", "",
                                  "SPEC.md, Death, recovery and expedition rules"};
    snprintf(rules[n++].to, sizeof(buf), "private recovery %d ticks then bank; respawn %d ticks",
             m->recovery_ticks, m->respawn_ticks);
    rules[n] = (typeof(rules[0])){"NPC drop ownership", "60 s private, then public", "",
                                  "SPEC.md, Loot ownership"};
    snprintf(rules[n++].to, sizeof(buf), "%d ticks private, then %d public", m->private_ticks,
             m->public_ticks);
    rules[n] = (typeof(rules[0])){"drop tables", "inherited tables", "",
                                  "SPEC.md, Ordinary encounters and drop tables"};
    snprintf(rules[n++].to, sizeof(buf), "%d closed mode tables", m->drop_count);
    rules[n] = (typeof(rules[0])){"shops", "stock-based OSRS prices; sales restock the shop", "",
                                  "SPEC.md, Shops, prices and economic invariants"};
    snprintf(rules[n++].to, sizeof(buf), "%d fixed-price shops; sales remove items", m->shop_count);
    rules[n] = (typeof(rules[0])){"gathering, fishing and cooking", "canonical unlock levels", "",
                                  "band rebase; success curves read at level + offset"};
    snprintf(rules[n++].to, sizeof(buf), "%d gather, %d fishing, %d cooking rules", m->gather_count,
             m->fish_count, m->cook_count);
    for (int i = 0; i < n; i++) {
        fprintf(f, "  {\"rule\": ");
        json_string(f, rules[i].rule);
        fprintf(f, ", \"from\": ");
        json_string(f, rules[i].from);
        fprintf(f, ", \"to\": ");
        json_string(f, rules[i].to);
        fprintf(f, ", \"why\": ");
        json_string(f, rules[i].why);
        fprintf(f, "}%s\n", i + 1 < n ? "," : "");
    }
    fprintf(f, " ],\n \"overrides\": [\n");
    // World rules the content installs: rebased gathering, fishing and cooking levels and the
    // fixed shop prices (inherited: the item's canonical value).
    RcModeLedger extra = {0};
    char from[32], to[96], name[64];

    extra.cap = m->gather_count + m->fish_count * RC_FISH_MAX_CATCHES + m->cook_count
              + RC_MODE_MAX_SHOPS * RC_MODE_MAX_STOCK;
    extra.rows = calloc((size_t)extra.cap, sizeof(*extra.rows));
    for (int i = 0; extra.rows && i < m->gather_count; i++) {
        const RcGatherRule *g = &m->gather[i];
        if (!g->level_offset) continue;
        snprintf(from, sizeof(from), "%d", g->level + g->level_offset);
        snprintf(to, sizeof(to), "%d (success curve read at level + %d)", g->level, g->level_offset);
        add_row(&extra, "gather", (int)g->obj_id, item_name((int)g->product), "level", from, to);
    }
    for (int i = 0; extra.rows && i < m->fish_count; i++)
        for (int c = 0; c < m->fish[i].catch_count; c++) {
            const RcFishCatch *x = &m->fish[i].catches[c];
            if (!x->level_offset) continue;
            snprintf(from, sizeof(from), "%d", x->level + x->level_offset);
            snprintf(to, sizeof(to), "%d (curve at level + %d)", x->level, x->level_offset);
            add_row(&extra, "fish", (int)x->product, item_name((int)x->product), "level", from, to);
        }
    for (int i = 0; extra.rows && i < m->cook_count; i++) {
        const RcCookRule *c = &m->cook[i];
        if (!c->level_offset) continue;
        snprintf(from, sizeof(from), "%d", c->level + c->level_offset);
        snprintf(to, sizeof(to), "%d (burn curve at level + %d)", c->level, c->level_offset);
        add_row(&extra, "cook", (int)c->cooked, item_name((int)c->cooked), "level", from, to);
    }
    for (int s = 0; extra.rows && s < m->shop_count; s++)
        for (int i = 0; i < m->shops[s].stock_count; i++) {
            const RcModeStock *st = &m->shops[s].stock[i];
            const RcItemDef *d = rc_item_def_get(st->item);
            snprintf(from, sizeof(from), "value %d", d ? d->value : 0);
            snprintf(to, sizeof(to), "%d coins, stock %d, +%d per %d ticks", st->price, st->cap,
                     st->qty, st->ticks);
            snprintf(name, sizeof(name), "%s", item_name(st->item));
            add_row(&extra, "shop", st->item, name, m->shops[s].name, from, to);
        }
    for (int i = 0; i < extra.count; i++) {
        const RcModeOverride *o = &extra.rows[i];
        const char *why = !strcmp(o->kind, "shop")
            ? "fixed mode prices and stock (SPEC.md, Shops, prices and economic invariants)"
            : "band rebase of an unlock level (SPEC.md, Closed production chains)";
        fprintf(f, "  {\"kind\": ");
        json_string(f, o->kind);
        fprintf(f, ", \"id\": %d, \"name\": ", o->id);
        json_string(f, o->name);
        fprintf(f, ", \"field\": ");
        json_string(f, o->field);
        fprintf(f, ", \"from\": ");
        json_string(f, o->from);
        fprintf(f, ", \"to\": ");
        json_string(f, o->to);
        fprintf(f, ", \"why\": ");
        json_string(f, why);
        fprintf(f, "},\n");
    }
    free(extra.rows);
    for (int i = 0; ledger && i < ledger->count; i++) {
        const RcModeOverride *o = &ledger->rows[i];
        fprintf(f, "  {\"kind\": ");
        json_string(f, o->kind);
        fprintf(f, ", \"id\": %d, \"name\": ", o->id);
        json_string(f, o->name);
        fprintf(f, ", \"field\": ");
        json_string(f, o->field);
        fprintf(f, ", \"from\": ");
        json_string(f, o->from);
        fprintf(f, ", \"to\": ");
        json_string(f, o->to);
        fprintf(f, ", \"why\": ");
        json_string(f, reason(o));
        fprintf(f, "}%s\n", i + 1 < ledger->count ? "," : "");
    }
    fprintf(f, " ]\n}\n");
    fclose(f);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : NULL;
    RcGymLayout layout;
    if (!rc_gym_layout_load_mode(mode, &layout)) {
        fprintf(stderr, "rs-gym-validate: mode %s is not built (tools/build_mode.py)\n",
                rc_gym_mode_name(mode));
        return 2;
    }
    if (!layout.has_rules) {
        fprintf(stderr, "rs-gym-validate: mode %s has no rules to validate\n", layout.mode);
        return 2;
    }
    mkdir("build", 0755);
    mkdir("build/reports", 0755);
    char path[512];
    snprintf(path, sizeof(path), "build/reports/%s-validation.txt", layout.mode);
    report = fopen(path, "w");
    if (!report) {
        fprintf(stderr, "rs-gym-validate: cannot write %s\n", path);
        return 2;
    }
    RcWorldConfig cfg = rc_gym_world_config(&layout, 1);
    RcWorld *w = rc_world_create_config(&cfg);
    const RcModeRules *m = rc_mode_active();
    if (!w || !m) {
        fprintf(stderr, "rs-gym-validate: the %s world did not load\n", layout.mode);
        return 1;
    }
    note("mode %s v%d: cap %d, xp x%d, up to %d players", m->name, m->version, m->level_cap,
         m->xp_multiplier, m->max_players);
    CHECK(!strcmp(m->name, layout.mode), "rules name %s, layout mode %s", m->name, layout.mode);
    CHECK(rc_world_install_content(w, rc_content_register_all), "content registration failed");
    CHECK(rc_world_relocate_player(w, layout.start_x, layout.start_y, 0), "cannot place a player at "
          "the start tile");
    collect_reachable(m);
    check_items(m);
    check_magic_prayer(m);
    check_ammunition(m);
    check_world(m, &layout, w);
    snprintf(path, sizeof(path), "modes/%s/override-ledger.json", layout.mode);
    const RcModeLedger *ledger = rc_mode_active_ledger();
    write_ledger(path, m, ledger);
    int overrides = ledger ? ledger->count : 0;
    note("ledger: %d definition overrides in %s", overrides, path);
    note("%d checks, %d failed", checks, failures);
    fclose(report);
    rc_world_destroy(w);  // releases the game data, and with it the rules and ledger
    printf("rs-gym-validate %s: %d checks, %d failed; ledger %s (%d overrides)\n", layout.mode, checks,
           failures, path, overrides);
    return failures ? 1 : 0;
}
