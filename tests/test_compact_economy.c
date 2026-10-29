// The compact_osrs_v1 production chains through real interactions (run tools/build_mode.py first).
// A fresh character walks the reference map from the bank and, with only player commands, mines,
// smelts and smiths arrowtips, chops and fletches, picks flax and spins a bow string, strings a
// shortbow, assembles arrows, kills a cow, has Ellis tan the hide, sews a leather body, cooks,
// buries bones, nets shrimps, mines essence and binds air runes, and trades with the general store.
// A second agent, given level 30 and seeded materials as test setup, makes every band-15 and band-30
// product at its station, the bone weapons, and uses Historian Aldo's service, in the same world.
#include "../rc-core/api.h"
#include "../rc-core/items.h"
#include "../rc-core/mode.h"
#include "../rc-core/players.h"
#include "../rc-core/skills.h"
#include "../rc-content/content.h"
#include "../rc-env/script.h"
#include "gym_test_config.h"

#include <stdlib.h>
#include <string.h>

enum {
    SKIP = 77,
    COINS = 995, BRONZE_AXE = 1351, BRONZE_PICKAXE = 1265, HAMMER = 2347, KNIFE = 946,
    NEEDLE = 1733, THREAD = 1734, FEATHER = 314, SMALL_NET = 303,
    COPPER_ORE = 436, TIN_ORE = 438, BRONZE_BAR = 2349, BRONZE_ARROWTIPS = 39,
    LOGS = 1511, ARROW_SHAFT = 52, HEADLESS_ARROW = 53, BRONZE_ARROW = 882,
    SHORTBOW_U = 50, SHORTBOW = 841, FLAX = 1779, BOW_STRING = 1777,
    COW = 2790, COWHIDE = 1739, RAW_BEEF = 2132, COOKED_MEAT = 2142, BURNT_MEAT = 2146,
    BONES = 526, LEATHER = 1741, LEATHER_BODY = 1129, RAW_SHRIMPS = 317, NET_SPOT = 1525,
    PURE_ESSENCE = 7936, AIR_RUNE = 556, ESSENCE_ROCK = 34773, AIR_ALTAR = 34760,
    SPINNING_WHEEL = 14889, FURNACE = 24009, ANVIL = 39620, RANGE = 7183,
    ELLIS = 3231, GENERAL_STORE = 2813,
    CHISEL = 1755, FLY_ROD = 309, LOBSTER_POT = 301, MITHRIL_AXE = 1355, MITHRIL_PICKAXE = 1273,
    IRON_ORE = 440, COAL = 453, MITHRIL_ORE = 447, STEEL_BAR = 2353, MITHRIL_BAR = 2359,
    STEEL_STUDS = 2370, MITHRIL_ARROWTIPS = 42, MITHRIL_ARROW = 888, OAK_LOGS = 1521,
    WILLOW_LOGS = 1519, OAK_SHORTBOW_U = 54, WILLOW_SHORTBOW_U = 60, OAK_SHORTBOW = 843,
    WILLOW_SHORTBOW = 849, HARD_LEATHER = 1743, HARDLEATHER_BODY = 1131, STUDDED_BODY = 1133,
    LEATHER_CHAPS = 1095, STUDDED_CHAPS = 1097, SPINE = 28798, MITHRIL_MACE = 1428,
    STAFF_OF_AIR = 1381, CHAOS_RUNE = 562, DEATH_RUNE = 560, BONE_MACE = 28792,
    BONE_SHORTBOW = 28794, BONE_STAFF = 28796, RAW_TROUT = 335, TROUT = 333, RAW_LOBSTER = 377,
    LOBSTER = 379, ROD_SPOT = 1526, CAGE_SPOT = 1519, CHAOS_ALTAR = 34769, DEATH_ALTAR = 34770,
    ALDO = 7615,
};

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int lm_x(const char *name) {
    for (int i = 0; i < gym_layout.landmark_count; i++)
        if (!strcmp(gym_layout.landmarks[i].name, name)) return gym_layout.landmarks[i].x;
    fprintf(stderr, "no landmark %s\n", name);
    exit(1);
}

static int lm_y(const char *name) {
    for (int i = 0; i < gym_layout.landmark_count; i++)
        if (!strcmp(gym_layout.landmarks[i].name, name)) return gym_layout.landmarks[i].y;
    exit(1);
}

#define GO(a, name) do { if (!sc_walk(a, lm_x(name), lm_y(name), 2)) \
    sc_fail(a, "could not walk to %s", name); } while (0)
#define NEED(cond, ...) do { if (!(cond)) sc_fail(a, __VA_ARGS__); } while (0)

static int station(ScAgent *a, int id, const char *option) {
    int ids[] = {id}, x, y;
    if (!sc_find_loc(a, ids, 1, 24, &x, &y, NULL)) {
        sc_note(a, "no loc %d near %d,%d", id, sc_player(a)->x, sc_player(a)->y);
        return 0;
    }
    if (sc_use_loc(a, id, x, y, option)) return 1;
    const RcPlayer *p = sc_player(a);
    sc_note(a, "%s on loc %d at %d,%d failed from %d,%d (failure %d, op %d): %s", option, id, x, y,
            p->x, p->y, p->interaction.last_failure, sc_loc_option(a, id, x, y, option),
            p->interaction_outcome.message);
    return 0;
}

static void chains(ScAgent *a, void *arg) {
    (void)arg;
    GO(a, "bank");
    NEED(sc_bank_open(a), "bank did not open");
    int tools[] = {BRONZE_AXE, BRONZE_PICKAXE, HAMMER, KNIFE, NEEDLE, SMALL_NET};
    for (int i = 0; i < 6; i++) NEED(sc_withdraw(a, tools[i], 1) == 1, "withdraw tool %d", tools[i]);
    NEED(sc_withdraw(a, THREAD, 5) == 5 && sc_withdraw(a, FEATHER, 30) == 30, "thread, feathers");
    NEED(sc_withdraw(a, COINS, 100) == 100, "coins");
    sc_bank_close(a);

    GO(a, "quarry");
    int copper[] = {10943, 11161}, tin[] = {11360, 11361};
    NEED(sc_gather(a, copper, 2, "Mine", COPPER_ORE, 1, 20), "no copper ore");
    NEED(sc_gather(a, tin, 2, "Mine", TIN_ORE, 1, 20), "no tin ore");
    sc_note(a, "mined copper and tin");

    GO(a, "trees");
    int trees[] = {1276, 1278};
    NEED(sc_gather(a, trees, 2, "Chop down", LOGS, 2, 20), "no logs");
    NEED(sc_use_items(a, KNIFE, LOGS) && sc_make(a, ARROW_SHAFT, 1) == 15, "arrow shafts");
    NEED(sc_use_items(a, KNIFE, LOGS) && sc_make(a, SHORTBOW_U, 1) == 1, "unstrung shortbow");
    NEED(sc_use_items(a, ARROW_SHAFT, FEATHER) && sc_make(a, HEADLESS_ARROW, 1) == 15,
         "headless arrows");
    sc_note(a, "fletched");

    GO(a, "flax");
    int flax[] = {14896};
    NEED(sc_gather(a, flax, 1, "Pick", FLAX, 1, 20), "no flax");
    GO(a, "spinning_wheel");
    NEED(station(a, SPINNING_WHEEL, "Spin") && sc_make(a, BOW_STRING, 1) == 1, "bow string");
    NEED(sc_use_items(a, BOW_STRING, SHORTBOW_U) && sc_make(a, SHORTBOW, 1) == 1, "shortbow");
    sc_note(a, "strung a shortbow");

    GO(a, "smithy");
    NEED(station(a, FURNACE, "Smelt") && sc_make(a, BRONZE_BAR, 1) == 1, "bronze bar");
    NEED(station(a, ANVIL, "Smith") && sc_make(a, BRONZE_ARROWTIPS, 1) == 15, "arrowtips");
    NEED(sc_use_items(a, HEADLESS_ARROW, BRONZE_ARROWTIPS) && sc_make(a, BRONZE_ARROW, 1) == 15,
         "bronze arrows");
    sc_note(a, "smithed and assembled arrows");

    GO(a, "cows");
    NEED(sc_kill(a, COW, 30, 40) == 1, "no cow kill");
    int loot[] = {COWHIDE, RAW_BEEF, BONES};
    sc_pickup(a, loot, 3, 6);
    NEED(sc_count(a, COWHIDE) == 1 && sc_count(a, RAW_BEEF) == 1, "cow drop not picked up");
    NEED(sc_bury_all(a) == 1, "bones not buried");
    sc_note(a, "killed a cow");

    GO(a, "ellis");
    NEED(sc_talk(a, sc_find_npc(a, ELLIS, 10, 0), "Trade") && sc_make(a, LEATHER, 1) == 1,
         "tanning");
    NEED(sc_use_items(a, NEEDLE, LEATHER) && sc_make(a, LEATHER_BODY, 1) == 1, "leather body");
    sc_note(a, "tanned and sewed a leather body");

    GO(a, "range");
    NEED(station(a, RANGE, "Cook"), "range");
    for (int t = 0; t < 20 && sc_count(a, RAW_BEEF); t++) sc_wait(a, 1);
    NEED(sc_count(a, COOKED_MEAT) + sc_count(a, BURNT_MEAT) == 1, "beef not cooked");

    GO(a, "cows");
    NEED(sc_fish(a, NET_SPOT, "Small Net", RAW_SHRIMPS, 1, 60), "no shrimps");
    sc_note(a, "netted shrimps");

    GO(a, "essence");
    int essence[] = {ESSENCE_ROCK};
    NEED(sc_gather(a, essence, 1, "Mine", PURE_ESSENCE, 3, 20), "no pure essence");
    GO(a, "air_altar");
    NEED(station(a, AIR_ALTAR, "Craft-rune"), "air altar");
    for (int t = 0; t < 10 && sc_count(a, PURE_ESSENCE); t++) sc_wait(a, 1);
    NEED(sc_count(a, AIR_RUNE) >= 3 && !sc_count(a, PURE_ESSENCE), "air runes");
    sc_note(a, "bound air runes");

    GO(a, "general_store");
    NEED(sc_shop_open(a, GENERAL_STORE), "general store");
    int coins = sc_count(a, COINS);
    NEED(sc_sell(a, LEATHER_BODY, 1) == 1 && sc_count(a, COINS) == coins + 20, "selling the body");
    NEED(sc_buy(a, THREAD, 2) == 2 && sc_count(a, COINS) == coins + 18, "buying thread");
    sc_shop_close(a);
}

#define SURFACE(v) ((v) + 128)  // surface area tiles to world tiles

static void withdraw(ScAgent *a, int item, int qty) {
    if (sc_withdraw(a, item, qty) != qty) sc_fail(a, "could not withdraw %d of %d", qty, item);
}

static void high_bands(ScAgent *a, void *arg) {
    (void)arg;
    GO(a, "bank");
    NEED(sc_bank_open(a), "bank did not open");
    withdraw(a, HAMMER, 1);
    withdraw(a, KNIFE, 1);
    withdraw(a, MITHRIL_AXE, 1);
    withdraw(a, IRON_ORE, 2);
    withdraw(a, COAL, 8);
    withdraw(a, MITHRIL_ORE, 1);
    withdraw(a, BOW_STRING, 2);
    withdraw(a, HEADLESS_ARROW, 15);
    sc_bank_close(a);

    GO(a, "smithy");
    NEED(station(a, FURNACE, "Smelt") && sc_make(a, STEEL_BAR, 2) == 2, "steel bars");
    NEED(station(a, FURNACE, "Smelt") && sc_make(a, MITHRIL_BAR, 1) == 1, "mithril bar");
    NEED(station(a, ANVIL, "Smith") && sc_make(a, STEEL_STUDS, 2) == 2, "steel studs");
    NEED(station(a, ANVIL, "Smith") && sc_make(a, MITHRIL_ARROWTIPS, 1) == 15, "mithril arrowtips");
    NEED(sc_use_items(a, HEADLESS_ARROW, MITHRIL_ARROWTIPS) && sc_make(a, MITHRIL_ARROW, 1) == 15,
         "mithril arrows");
    sc_note(a, "band 15/30 smithing");

    GO(a, "oaks");
    int oaks[] = {10820}, willows[] = {10819, 10829, 10831, 10833};
    NEED(sc_gather(a, oaks, 1, "Chop down", OAK_LOGS, 1, 20), "no oak logs");
    NEED(sc_use_items(a, KNIFE, OAK_LOGS) && sc_make(a, OAK_SHORTBOW_U, 1) == 1, "oak shortbow (u)");
    NEED(sc_use_items(a, BOW_STRING, OAK_SHORTBOW_U) && sc_make(a, OAK_SHORTBOW, 1) == 1,
         "oak shortbow");
    GO(a, "outer_area");
    NEED(sc_gather(a, willows, 4, "Chop down", WILLOW_LOGS, 1, 24), "no willow logs");
    NEED(sc_use_items(a, KNIFE, WILLOW_LOGS) && sc_make(a, WILLOW_SHORTBOW_U, 1) == 1,
         "willow shortbow (u)");
    NEED(sc_use_items(a, BOW_STRING, WILLOW_SHORTBOW_U) && sc_make(a, WILLOW_SHORTBOW, 1) == 1,
         "willow shortbow");
    sc_note(a, "band 15/30 fletching");

    GO(a, "bank");
    NEED(sc_bank_open(a), "bank did not open");
    sc_deposit_all_except(a, (int[]){HAMMER, KNIFE, STEEL_STUDS, WILLOW_SHORTBOW}, 4);
    withdraw(a, NEEDLE, 1);
    withdraw(a, THREAD, 2);
    withdraw(a, CHISEL, 1);
    withdraw(a, COWHIDE, 1);
    withdraw(a, LEATHER_BODY, 1);
    withdraw(a, LEATHER_CHAPS, 1);
    withdraw(a, SPINE, 4);
    withdraw(a, MITHRIL_MACE, 2);
    withdraw(a, STAFF_OF_AIR, 1);
    withdraw(a, CHAOS_RUNE, 200);
    withdraw(a, COINS, 1000);
    withdraw(a, PURE_ESSENCE, 1);
    withdraw(a, MITHRIL_PICKAXE, 1);
    withdraw(a, FLY_ROD, 1);
    withdraw(a, FEATHER, 20);
    withdraw(a, LOBSTER_POT, 1);
    sc_bank_close(a);

    GO(a, "ellis");
    NEED(sc_talk(a, sc_find_npc(a, ELLIS, 10, 0), "Trade") && sc_make(a, HARD_LEATHER, 1) == 1,
         "hard leather");
    NEED(sc_count(a, COINS) == 997, "tanning hard leather cost %d", 1000 - sc_count(a, COINS));
    NEED(sc_use_items(a, NEEDLE, HARD_LEATHER) && sc_make(a, HARDLEATHER_BODY, 1) == 1,
         "hardleather body");
    NEED(sc_use_items(a, LEATHER_BODY, STEEL_STUDS) && sc_make(a, STUDDED_BODY, 1) == 1,
         "studded body");
    NEED(sc_use_items(a, LEATHER_CHAPS, STEEL_STUDS) && sc_make(a, STUDDED_CHAPS, 1) == 1,
         "studded chaps");
    sc_note(a, "band 15/30 crafting");

    GO(a, "smithy");
    int smithing = sc_player(a)->skills.xp[SKILL_SMITHING];
    NEED(station(a, ANVIL, "Smith") && sc_make(a, BONE_MACE, 1) == 1, "bone mace");
    NEED(sc_player(a)->skills.xp[SKILL_SMITHING] - smithing == 300 * 8, "bone mace XP");
    NEED(sc_use_items(a, KNIFE, SPINE) && sc_make(a, BONE_SHORTBOW, 1) == 1, "bone shortbow");
    NEED(sc_use_items(a, CHISEL, SPINE) && sc_make(a, BONE_STAFF, 1) == 1, "bone staff");
    NEED(sc_count(a, CHAOS_RUNE) == 100, "the bone staff took %d chaos runes",
         200 - sc_count(a, CHAOS_RUNE));
    GO(a, "dungeon_entrance");
    smithing = sc_player(a)->skills.xp[SKILL_SMITHING];
    int coins = sc_count(a, COINS);
    NEED(sc_talk(a, sc_find_npc(a, ALDO, 10, 0), "Trade") && sc_make(a, BONE_MACE, 1) == 1,
         "Aldo's bone mace");
    NEED(coins - sc_count(a, COINS) == 100 && sc_player(a)->skills.xp[SKILL_SMITHING] == smithing,
         "Aldo charged %d coins or gave XP", coins - sc_count(a, COINS));
    sc_note(a, "bone weapons");

    GO(a, "chaos_altar");
    NEED(station(a, CHAOS_ALTAR, "Craft-rune"), "chaos altar");
    for (int t = 0; t < 10 && sc_count(a, PURE_ESSENCE); t++) sc_wait(a, 1);
    NEED(sc_count(a, CHAOS_RUNE) == 101, "chaos rune");
    GO(a, "lobster");
    NEED(sc_fish(a, CAGE_SPOT, "Cage", RAW_LOBSTER, 1, 40), "no lobster");
    GO(a, "essence");
    int essence[] = {ESSENCE_ROCK};
    NEED(sc_gather(a, essence, 1, "Mine", PURE_ESSENCE, 1, 20), "no pure essence");
    GO(a, "death_altar");
    NEED(station(a, DEATH_ALTAR, "Craft-rune"), "death altar");
    for (int t = 0; t < 10 && sc_count(a, PURE_ESSENCE); t++) sc_wait(a, 1);
    NEED(sc_count(a, DEATH_RUNE) == 1, "death rune");
    sc_note(a, "band 15/30 runecraft");

    NEED(sc_walk(a, SURFACE(30), SURFACE(96), 3), "walk to the river");
    NEED(sc_fish(a, ROD_SPOT, "Lure", RAW_TROUT, 1, 30), "no trout");
    GO(a, "range");
    for (int round = 0; round < 2; round++) {  // a Cook click cooks one kind of food
        NEED(station(a, RANGE, "Cook"), "range");
        for (int t = 0; t < 12 && sc_player(a)->skill_job.kind; t++) sc_wait(a, 1);
    }
    NEED(!sc_count(a, RAW_TROUT) && !sc_count(a, RAW_LOBSTER), "fish not cooked");
    sc_note(a, "band 15/30 fishing and cooking: trout %d lobster %d", sc_count(a, TROUT),
            sc_count(a, LOBSTER));
}

// Test setup for the band-30 agent: level 30 everywhere and seeded materials in its bank.
static void seed_high_bands(RcWorld *w) {
    RcPlayer *p = w->player;
    for (int s = 0; s < SKILL_COUNT; s++) {
        p->skills.base_level[s] = p->skills.boosted_level[s] = 30;
        p->skills.xp[s] = RC_XP_TABLE[29];
    }
    p->max_hp = p->current_hp = 300;
    static const int seed[][2] = {
        {HAMMER, 1}, {KNIFE, 1}, {NEEDLE, 1}, {CHISEL, 1}, {THREAD, 10}, {FEATHER, 60},
        {FLY_ROD, 1}, {LOBSTER_POT, 1}, {MITHRIL_AXE, 1}, {MITHRIL_PICKAXE, 1}, {IRON_ORE, 2},
        {COAL, 8}, {MITHRIL_ORE, 1}, {BOW_STRING, 2}, {HEADLESS_ARROW, 15}, {LEATHER_BODY, 1},
        {LEATHER_CHAPS, 1}, {COWHIDE, 1}, {SPINE, 4}, {STAFF_OF_AIR, 1}, {MITHRIL_MACE, 2},
        {CHAOS_RUNE, 200}, {PURE_ESSENCE, 1}, {COINS, 1000},
    };
    for (unsigned i = 0; i < sizeof(seed) / sizeof(seed[0]); i++) rc_bank_add_item(w, seed[i][0], seed[i][1]);
}

int main(void) {
    if (!gym_mode_built("compact_osrs_v1")) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config_mode("compact_osrs_v1", 2);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w || !rc_world_install_content(w, rc_content_register_all)) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        rc_world_select_player(w, i);
        CHECK(rc_world_relocate_player(w, GYM_START_X + i, GYM_START_Y, 0), "place player %d", i);
        rc_content_compact_new_character(w);
    }
    seed_high_bands(w);  // player 1
    ScRun run;
    FILE *log = getenv("RS_GYM_SCRIPT_LOG") ? stderr : NULL;
    sc_init(&run, w, 2, log);
    sc_start(&run, 0, chains, NULL);
    sc_start(&run, 1, high_bands, NULL);
    int ticks = sc_run(&run, w->tick + 8000);
    for (int i = 0; i < 2; i++) {
        CHECK(run.agents[i].done && !run.agents[i].failed, "agent %d stopped at tick %d: %s", i, ticks,
              run.agents[i].failed ? run.agents[i].failure : "still running");
        CHECK(run.agents[i].deaths == 0, "agent %d died", i);
    }
    const RcPlayer *p = &w->players[0];
    printf("economy chains: %d ticks; xp mining %d smithing %d fletching %d crafting %d runecraft %d "
           "cooking %d prayer %d fishing %d\n", ticks, p->skills.xp[SKILL_MINING],
           p->skills.xp[SKILL_SMITHING], p->skills.xp[SKILL_FLETCHING], p->skills.xp[SKILL_CRAFTING],
           p->skills.xp[SKILL_RUNECRAFT], p->skills.xp[SKILL_COOKING], p->skills.xp[SKILL_PRAYER],
           p->skills.xp[SKILL_FISHING]);
    sc_free(&run);
    rc_world_destroy(w);
    if (failures) return 1;
    printf("economy chains: PASS\n");
    return 0;
}
