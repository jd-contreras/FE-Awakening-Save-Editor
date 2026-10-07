// PC-side tests for the portable save core. Build with tests/run_host_tests.sh.
//
//   host_test <save files...>   e.g. Chapter0 Chapter1 Chapter2 Map0
//
// For each file: load, list units with in-game stats, rebuild, reload and compare,
// then apply sample edits to a copy and check they survive a round trip.
// Also stress-tests the Huffman codec on synthetic data.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fesave.h"
#include "core/huffman.h"
#include "core/feglobal.h"
#include "core/feimport.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

// Values read off the real game's stat screen (Luma game patching OFF), 2026-10-02.
// File names are the Checkpoint dump names in "G:/dev/Checkpoint Save".
// base = 1: compare the zero-gain stats (fe_stats.base) instead of the current ones;
// used when the photo was taken before the unit leveled up.
typedef struct { const char *file, *name, *cls; int stats[FE_STAT_COUNT]; int base; } expected_unit;
static const expected_unit EXPECTED[] = {
    {"Chapter0", "Lucina",     "Lord (F)",      {60, 28, 20, 30, 32, 31, 25, 24}, 0},  // child, all capped
    {"Chapter0", "Morgan (M)", "Tactician (M)", {60, 25, 25, 28, 30, 32, 25, 25}, 0},  // child, not capped
    {"Chapter0", "Morgan (F)", "Tactician (F)", {60, 25, 25, 28, 30, 32, 25, 25}, 0},
    {"Chapter0", "Robin",      "Tactician (M)", {60, 24, 24, 27, 29, 31, 24, 24}, 0},  // Avatar, asset Spd / flaw HP
    {"Chapter2", "Bond",       "Tactician (M)", {25, 14, 13,  8, 12,  9,  6,  7}, 0},  // Avatar, asset Mag / flaw Def
    // Fresh vanilla game ("G:/dev/Chapter0 - Vanilla"): Robin asset Lck / flaw Skl, photographed at Lv1
    {"Chapter0 - Vanilla", "Robin",     "Tactician (M)",    {19,  6,  5,  4,  6,  8,  6,  4}, 1},
    // Lv1, no gains yet: official join stats (Serenes Forest)
    {"Chapter0 - Vanilla", "Chrom",     "Lord (M)",         {20,  7,  1,  8,  8,  5,  7,  1}, 0},
    {"Chapter0 - Vanilla", "Frederick", "Great Knight (M)", {28, 13,  2, 12, 10,  6, 14,  3}, 0},
};

static void check_expected(const char *path, const fe_save *s)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (size_t e = 0; e < sizeof(EXPECTED) / sizeof(EXPECTED[0]); e++) {
        const expected_unit *x = &EXPECTED[e];
        if (strcmp(base, x->file) != 0) continue;
        bool found = false;
        for (int i = 0; i < s->unit_count && !found; i++) {
            const fe_unit *u = &s->units[i];
            char name[48];
            fe_unit_name(s, u, name, sizeof(name));
            if (strcmp(name, x->name) || strcmp(fe_class_name(fe_unit_class_id(s, u)), x->cls)) continue;
            found = true;
            fe_stats st;
            fe_unit_stats(s, u, &st);
            int bad = 0;
            for (int k = 0; k < FE_STAT_COUNT; k++)
                if ((x->base ? st.base[k] : fe_stat_shown(&st, k)) != x->stats[k]) bad++;
            CHECK(bad == 0, "%s %s: %d stat(s) differ from the in-game screen", x->name, x->cls, bad);
            if (!bad) printf("  matches in-game screen: %s (%s)\n", x->name, x->cls);
        }
        CHECK(found, "expected unit %s (%s) not found", x->name, x->cls);
    }
}

static void list_units(const fe_save *s)
{
    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        char name[64];
        fe_unit_name(s, u, name, sizeof(name));
        fe_stats st;
        bool ok = fe_unit_stats(s, u, &st);
        printf("  [%-8s] %-14s %-18s Lv%2d/%d", fe_group_name(u->group), name,
               fe_class_name(fe_unit_class_id(s, u)), fe_unit_level(s, u), fe_unit_level_cap(s, u));
        if (ok) {
            for (int k = 0; k < FE_STAT_COUNT; k++) {
                printf(" %s %2d/%-2d", fe_stat_names[k], st.value[k], st.cap[k]);
                if (st.buff[k]) printf("(+%d)", st.buff[k]);
            }
            if (st.limit_breaker) printf(" LB");
        } else {
            printf(" (no stat data)");
        }
        printf("\n");
    }
}

static void test_save(const char *path)
{
    printf("=== %s\n", path);
    size_t len;
    uint8_t *file = read_file(path, &len);
    if (!file) { g_failures++; printf("  FAIL: cannot read file\n"); return; }

    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!fe_save_load(s, file, len, err, sizeof(err))) {
        g_failures++;
        printf("  FAIL: load: %s\n", err);
        free(s); free(file);
        return;
    }
    printf("  %s file, %u bytes decompressed, %d units, gold %u, renown %u\n",
           s->is_map ? "Map" : "Chapter", (unsigned)s->size, s->unit_count, (unsigned)fe_get_gold(s),
           (unsigned)fe_get_renown(s));
    CHECK(fe_get_renown(s) <= FE_RENOWN_MAX, "renown %u above the game cap", (unsigned)fe_get_renown(s));
    list_units(s);
    check_expected(path, s);

    // Formula self-check: between chapters every unit is at full HP, so the stored
    // current HP must equal the computed max HP (permanent + buffs).
    if (!s->is_map) {
        int checked = 0, bad = 0;
        for (int i = 0; i < s->unit_count; i++) {
            const fe_unit *u = &s->units[i];
            fe_stats st;
            if (!fe_unit_stats(s, u, &st)) continue;
            int cur = s->data[u->off + 0x14];
            int max = fe_stat_shown(&st, 0);
            checked++;
            if (cur != max) {
                char name[48];
                fe_unit_name(s, u, name, sizeof(name));
                printf("  HP CHECK: %-14s stored current HP %d, computed max %d%s%s\n", name, cur, max,
                       u->log_off >= 0 ? " (logbook)" : "", u->child_off >= 0 ? " (child)" : "");
                bad++;
            }
        }
        // Reported, not failed: cheat-edited saves (e.g. an all-Dancer army) can
        // legitimately disagree, and child units' formula is still unverified.
        printf("  HP self-check: %d/%d units match\n", checked - bad, checked);
    }

    // Unedited rebuild
    uint8_t *out; size_t out_len;
    bool built = fe_save_build(s, &out, &out_len, err, sizeof(err));
    CHECK(built, "build: %s", err);
    if (built) {
        printf("  rebuilt: %u bytes (original %u)%s\n", (unsigned)out_len, (unsigned)len,
               (out_len == len && memcmp(out, file, len) == 0) ? ", byte-identical to the game's file" : "");
        // Header (0xC0) and CRC must be identical even if the compressed stream differs
        CHECK(memcmp(out, file, FE_HEADER_US + 0x10) == 0, "header/COMP block differs from original");
        free(out);
    }

    // Edits on the first unit that has stat data
    const fe_unit *target = NULL;
    for (int i = 0; i < s->unit_count && !target; i++) {
        fe_stats st;
        if (fe_unit_stats(s, &s->units[i], &st)) target = &s->units[i];
    }
    if (target) {
        int idx = (int)(target - s->units);
        fe_stats before;
        fe_unit_stats(s, target, &before);

        fe_set_gold(s, 123456);
        uint32_t renown_before = fe_get_renown(s);
        fe_set_renown(s, 4321);
        CHECK(fe_get_renown(s) == 4321 && fe_get_gold(s) == 123456, "renown write touched gold");
        fe_set_renown(s, 5000000);
        CHECK(fe_get_renown(s) == FE_RENOWN_MAX, "renown not clamped");
        fe_set_renown(s, 4321);
        (void)renown_before;
        int want_str = before.value[1] + 1 <= before.cap[1] ? before.value[1] + 1 : before.value[1] - 1;
        int got_str = fe_unit_set_stat(s, target, 1, want_str);
        int got_hp = fe_unit_set_stat(s, target, 0, 1000);  // clamps to cap
        int got_lv = fe_unit_set_level(s, target, 5);

        CHECK(got_str == want_str, "set Str: wanted %d got %d", want_str, got_str);
        CHECK(got_hp == before.cap[0], "HP clamp: wanted %d got %d", before.cap[0], got_hp);

        built = fe_save_build(s, &out, &out_len, err, sizeof(err));
        CHECK(built, "build after edit: %s", err);
        if (built) {
            fe_save *r = malloc(sizeof(fe_save));
            if (fe_save_load(r, out, out_len, err, sizeof(err))) {
                fe_stats after;
                fe_unit_stats(r, &r->units[idx], &after);
                CHECK(fe_get_gold(r) == 123456, "gold after reload %u", (unsigned)fe_get_gold(r));
                CHECK(fe_get_renown(r) == 4321, "renown after reload %u", (unsigned)fe_get_renown(r));
                CHECK(after.value[1] == want_str, "Str after reload %d", after.value[1]);
                CHECK(after.value[0] == before.cap[0], "HP after reload %d", after.value[0]);
                CHECK(fe_unit_level(r, &r->units[idx]) == got_lv, "level after reload");
                CHECK(r->unit_count == s->unit_count, "unit count changed");
                // Nothing outside the edited bytes may change
                size_t diffs = 0;
                for (uint32_t i = 0; i < r->size; i++) {
                    bool expected = (i >= s->gold_off && i < s->gold_off + 8) ||
                                    (i >= target->off && i < target->off + 0x1A);
                    if (r->data[i] != s->data[i] && !expected) diffs++;
                }
                CHECK(diffs == 0, "%u unexpected byte changes", (unsigned)diffs);
                printf("  edit round trip ok: gold 123456, Str %d->%d, HP %d->%d, Lv ->%d\n",
                       before.value[1], want_str, before.value[0], got_hp, got_lv);
                fe_save_free(r);
            } else {
                CHECK(false, "reload after edit: %s", err);
            }
            free(r);
            free(out);
        }
    }

    fe_save_free(s);
    free(s);
    free(file);
}

// Skill editing: invariants on the real save, then edits on a copy.
static void test_skills(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }

    // Every equipped skill should be learned in a normal save
    int violations = 0;
    for (int i = 0; i < s->unit_count; i++) {
        int eq[FE_EQUIP_SLOTS];
        fe_unit_equipped(s, &s->units[i], eq);
        for (int k = 0; k < FE_EQUIP_SLOTS; k++)
            if (eq[k] && !fe_unit_is_learned(s, &s->units[i], eq[k])) violations++;
    }
    // Reported, not failed: cheat tools equip skills without learning them
    // (the cheat-edited all-Dancer save has 40 such slots). Normal saves have 0.
    printf("  skills: %d equipped-but-not-learned slot(s)\n", violations);

    // Pick a player unit with at least 2 free slots and stat data
    const fe_unit *u = NULL;
    for (int i = 0; i < s->unit_count && !u; i++) {
        const fe_unit *c = &s->units[i];
        int eq[FE_EQUIP_SLOTS], used = 0;
        fe_stats st;
        fe_unit_equipped(s, c, eq);
        for (int k = 0; k < FE_EQUIP_SLOTS; k++) used += eq[k] != 0;
        if ((c->group == FE_GROUP_ARMY || c->group == FE_GROUP_BLUE) && used <= 3 && fe_unit_stats(s, c, &st))
            u = c;
    }
    if (!u) { printf("  skills: no unit with free slots, edit test skipped (%d checked)\n", s->unit_count); goto done; }

    uint8_t *orig = malloc(s->size);
    memcpy(orig, s->data, s->size);
    int idx = (int)(u - s->units);
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    // Find a skill that is neither learned nor equipped (or just not equipped)
    int pick = 0;
    for (int id = FE_SKILL_FIRST; id <= FE_SKILL_LAST && !pick; id++)
        if (id != 1 && !fe_unit_is_equipped(s, u, id) && !fe_unit_is_learned(s, u, id)) pick = id;
    for (int id = FE_SKILL_FIRST; id <= FE_SKILL_LAST && !pick; id++)
        if (id != 1 && !fe_unit_is_equipped(s, u, id)) pick = id;

    int slot = fe_unit_equip(s, u, pick);
    CHECK(slot >= 0 && fe_unit_is_equipped(s, u, pick) && fe_unit_is_learned(s, u, pick),
          "equip %d: slot %d, equipped %d, learned %d", pick, slot, fe_unit_is_equipped(s, u, pick), fe_unit_is_learned(s, u, pick));
    CHECK(fe_unit_equip(s, u, pick) == -1, "equipping the same skill twice should fail");

    fe_unit_unequip(s, u, pick);
    CHECK(!fe_unit_is_equipped(s, u, pick) && fe_unit_is_learned(s, u, pick), "unequip should keep the skill learned");

    // Compaction: after removing the first equipped skill, no empty slot may precede a filled one
    int eq[FE_EQUIP_SLOTS];
    fe_unit_equip(s, u, pick);
    fe_unit_equipped(s, u, eq);
    int first = eq[0];
    fe_unit_unequip(s, u, first);
    fe_unit_equipped(s, u, eq);
    int seen_empty = 0, gap = 0;
    for (int k = 0; k < FE_EQUIP_SLOTS; k++) { if (!eq[k]) seen_empty = 1; else if (seen_empty) gap = 1; }
    CHECK(!gap, "empty slot before a filled slot after unequip");
    fe_unit_equip(s, u, first);

    // Unlearning an equipped skill unequips it
    fe_unit_set_learned(s, u, pick, false);
    CHECK(!fe_unit_is_equipped(s, u, pick) && !fe_unit_is_learned(s, u, pick), "unlearn should also unequip");

    // HP +5 (skill 1) keeps a full-HP unit at full HP, both ways
    fe_unit_learn_all(s, u);
    CHECK(fe_unit_learned_count(s, u) == FE_SKILL_LAST, "learn all: %d learned", fe_unit_learned_count(s, u));
    if (!fe_unit_is_equipped(s, u, 1)) {
        fe_stats st;
        fe_unit_stats(s, u, &st);
        int cur = s->data[u->off + 0x14];
        bool was_full = cur == fe_stat_shown(&st, 0);
        if (fe_unit_equip(s, u, 1) >= 0) {
            fe_unit_stats(s, u, &st);
            if (was_full) CHECK(s->data[u->off + 0x14] == fe_stat_shown(&st, 0), "HP +5 equip: current HP not kept full");
            fe_unit_unequip(s, u, 1);
            fe_unit_stats(s, u, &st);
            if (was_full) CHECK(s->data[u->off + 0x14] == fe_stat_shown(&st, 0), "HP +5 unequip: current HP not kept full");
        }
    }

    // Only the equipped slots, learned bits and current HP of this unit may change
    size_t stray = 0;
    for (uint32_t i = 0; i < s->size; i++) {
        bool ok = (i >= u->off + 0x33 && i < u->off + 0x3D) ||
                  (i >= u->flags_off + 0x2A && i < u->flags_off + 0x2A + 0x0D) || i == u->off + 0x14;
        if (s->data[i] != orig[i] && !ok) stray++;
    }
    CHECK(stray == 0, "%u bytes changed outside the skill fields", (unsigned)stray);

    // Round trip
    uint8_t *out; size_t out_len;
    if (fe_save_build(s, &out, &out_len, err, sizeof(err))) {
        fe_save *r = malloc(sizeof(fe_save));
        if (fe_save_load(r, out, out_len, err, sizeof(err))) {
            CHECK(fe_unit_learned_count(r, &r->units[idx]) == FE_SKILL_LAST, "learned skills lost on reload");
            CHECK(memcmp(r->data, s->data, s->size) == 0, "reloaded save differs");
            fe_save_free(r);
        } else CHECK(false, "reload after skill edit: %s", err);
        free(r);
        free(out);
    } else CHECK(false, "build after skill edit: %s", err);
    printf("  skills ok on %s: equip/unequip/compact/unlearn/learn-all/HP+5, round trip\n", name);
    free(orig);

done:
    fe_save_free(s);
    free(s);
    free(file);
}

// Class change: every class id is accepted, gains are kept, level is clamped,
// full HP stays full, nothing outside the class/level/EXP/HP bytes changes.
static void test_class(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }

    const fe_unit *u = NULL;
    for (int i = 0; i < s->unit_count && !u; i++) {
        fe_stats st;
        if ((s->units[i].group == FE_GROUP_ARMY || s->units[i].group == FE_GROUP_BLUE) &&
            fe_unit_stats(s, &s->units[i], &st) && s->data[s->units[i].off + 0x14] == fe_stat_shown(&st, 0))
            u = &s->units[i];
    }
    if (!u) goto done;

    uint8_t *orig = malloc(s->size);
    memcpy(orig, s->data, s->size);
    uint8_t gains[8];
    memcpy(gains, s->data + u->off + 0x0A, 8);
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    int bad_hp = 0, bad_lv = 0;
    for (int c = 0; c < FE_CLASS_COUNT; c++) {
        CHECK(fe_unit_set_class(s, u, c), "class %d rejected", c);
        fe_stats st;
        fe_unit_stats(s, u, &st);
        if (s->data[u->off + 0x14] != fe_stat_shown(&st, 0)) bad_hp++;
        if (fe_unit_level(s, u) > fe_unit_level_cap(s, u)) bad_lv++;
    }
    CHECK(!fe_unit_set_class(s, u, FE_CLASS_COUNT), "invalid class id accepted");
    CHECK(bad_hp == 0, "%d classes left a full-HP unit below full HP", bad_hp);
    CHECK(bad_lv == 0, "%d classes left the level above the cap", bad_lv);
    CHECK(memcmp(gains, s->data + u->off + 0x0A, 8) == 0, "class change altered stored gains");

    size_t stray = 0;
    for (uint32_t i = 0; i < s->size; i++) {
        bool ok = i == u->off + 0x03 || i == u->off + 0x12 || i == u->off + 0x13 || i == u->off + 0x14;
        if (s->data[i] != orig[i] && !ok) stray++;
    }
    CHECK(stray == 0, "%u bytes changed outside class/level/EXP/HP", (unsigned)stray);

    uint8_t *out; size_t out_len;
    if (fe_save_build(s, &out, &out_len, err, sizeof(err))) {
        fe_save *r = malloc(sizeof(fe_save));
        if (fe_save_load(r, out, out_len, err, sizeof(err))) {
            CHECK(memcmp(r->data, s->data, s->size) == 0, "reloaded save differs after class change");
            fe_save_free(r);
        } else CHECK(false, "reload after class change: %s", err);
        free(r);
        free(out);
    } else CHECK(false, "build after class change: %s", err);
    printf("  class ok on %s: all %d classes, gains kept, level clamped, HP kept full, round trip\n", name, FE_CLASS_COUNT);
    free(orig);
done:
    fe_save_free(s);
    free(s);
    free(file);
}

// Supports: symmetry report, then grow a short block on a copy and verify the
// whole save shifted correctly.
static void test_supports(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }

    int asym = 0;
    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        if (fe_find_unit(s, fe_unit_char_id(s, u)) != i) continue;
        uint16_t id = fe_unit_char_id(s, u);
        if (id >= FE_CHAR_COUNT) continue;
        for (int k = 0; k < fe_supports[id].count; k++) {
            int p = fe_find_unit(s, fe_supports[id].partners[k].partner);
            if (p < 0) continue;
            int a = fe_support_get(s, u, fe_supports[id].partners[k].partner);
            int b = fe_support_get(s, &s->units[p], id);
            if (b >= 0 && a != b) asym++;
        }
    }
    printf("  supports: %d asymmetric pair value(s)\n", asym);

    // Find unit A with a partner B (present in the save) stored beyond A's block length
    int ai = -1, bchar = -1, bi = -1;
    for (int i = 0; i < s->unit_count && ai < 0; i++) {
        const fe_unit *u = &s->units[i];
        uint16_t id = fe_unit_char_id(s, u);
        if (id >= FE_CHAR_COUNT || fe_find_unit(s, id) != i) continue;
        int stored = s->data[u->off + 0x43];
        for (int k = stored; k < fe_supports[id].count; k++) {
            int p = fe_find_unit(s, fe_supports[id].partners[k].partner);
            if (p >= 0 && p != i) { ai = i; bchar = fe_supports[id].partners[k].partner; bi = p; break; }
        }
    }
    if (ai < 0) { printf("  supports: no short block with a present partner, grow test skipped\n"); goto done; }

    // Snapshot before
    fe_save before = *s;  // struct copy for offsets; data copied separately
    uint8_t *old = malloc(s->size);
    memcpy(old, s->data, s->size);
    uint32_t old_size = s->size, old_unit_end = s->unit_end, old_gold = fe_get_gold(s);
    int achar = fe_unit_char_id(s, &s->units[ai]);
    int b_lists_a = fe_support_get(s, &s->units[bi], achar) >= 0;

    char an[48], bn[48];
    fe_unit_name(s, &s->units[ai], an, sizeof(an));
    fe_unit_name(s, &s->units[bi], bn, sizeof(bn));

    bool ok = fe_support_set(s, ai, bchar, 7, err, sizeof(err));
    CHECK(ok, "support set failed: %s", err);
    if (ok) {
        CHECK(fe_support_get(s, &s->units[ai], bchar) == 7, "A side not set");
        if (b_lists_a) CHECK(fe_support_get(s, &s->units[bi], achar) == 7, "B side not set");
        CHECK(s->unit_count == before.unit_count, "unit count changed");
        CHECK(fe_get_gold(s) == old_gold, "gold changed");
        uint32_t grew = s->size - old_size;
        CHECK(s->unit_end == old_unit_end + grew, "UNIT block end not shifted by %u", (unsigned)grew);
        // Everything after the UNIT block is identical, just moved
        CHECK(memcmp(s->data + s->unit_end, old + old_unit_end, old_size - old_unit_end) == 0,
              "blocks after UNIT changed");
        // Everything before the first changed unit is identical except the index
        uint32_t first = before.units[ai].off < before.units[bi].off ? before.units[ai].off : before.units[bi].off;
        uint32_t idx_lo = s->hdr_size + 4, idx_hi = s->hdr_size + 4 + 36;
        size_t stray = 0;
        for (uint32_t i = 0; i < first; i++)
            if (s->data[i] != old[i] && !(i >= idx_lo && i < idx_hi)) stray++;
        CHECK(stray == 0, "%u bytes changed before the edited units", (unsigned)stray);
        // Every other unit record is byte-identical
        int diff_units = 0;
        for (int i = 0; i < s->unit_count; i++) {
            if (i == ai || i == bi) continue;
            if (s->units[i].size != before.units[i].size ||
                memcmp(s->data + s->units[i].off, old + before.units[i].off, s->units[i].size) != 0)
                diff_units++;
        }
        CHECK(diff_units == 0, "%d other unit records changed", diff_units);

        uint8_t *out; size_t out_len;
        if (fe_save_build(s, &out, &out_len, err, sizeof(err))) {
            fe_save *r = malloc(sizeof(fe_save));
            if (fe_save_load(r, out, out_len, err, sizeof(err))) {
                CHECK(fe_support_get(r, &r->units[ai], bchar) == 7, "support lost on reload");
                CHECK(memcmp(r->data, s->data, s->size) == 0, "reloaded save differs");
                fe_save_free(r);
            } else CHECK(false, "reload after support edit: %s", err);
            free(r);
            free(out);
        } else CHECK(false, "build after support edit: %s", err);
        printf("  supports ok: %s <-> %s set to 7, save grew %u bytes, all other data intact, round trip\n",
               an, bn, (unsigned)grew);
    }
    free(old);
done:
    fe_save_free(s);
    free(s);
    free(file);
}

// Rank names checked against the game's Support screen (slot 3, 2026-10-02)
static void test_support_ranks(void)
{
    printf("=== Support ranks\n");
    struct { int type, value; const char *want; } cases[] = {
        {0, 3, "C ready"},  // Frederick-Bond: the only "!" pair
        {0, 4, "C"},        // Chrom-Frederick, Lon'qu-Bond: C reached
        {3, 3, "C"},        // Chrom-Sumia (fast): C reached
        {0, 2, "-"},        // Chrom-Lissa: nothing yet
        {0, 8, "C"}, {0, 9, "B ready"}, {0, 17, "A ready"}, {0, 18, "A"}, {0, 40, "A"},
        {1, 16, "A"}, {1, 21, "S ready"}, {1, 22, "S"}, {2, 19, "S ready"}, {3, 17, "S ready"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char got[16];
        fe_support_rank_name(cases[i].type, cases[i].value, got, sizeof(got));
        CHECK(strcmp(got, cases[i].want) == 0, "type %d value %d: '%s' want '%s'", cases[i].type, cases[i].value, got, cases[i].want);
    }
    CHECK(fe_support_next_ready(0, 0) == 3 && fe_support_next_ready(0, 4) == 9 && fe_support_next_ready(0, 17) == -1, "next ready (non-romantic)");
    CHECK(fe_support_next_ready(1, 16) == 21 && fe_support_prev_ready(1, 16) == 15 && fe_support_prev_ready(1, 4) == 0, "next/prev ready (slow)");
    printf("  %d rank cases ok\n", (int)(sizeof(cases) / sizeof(cases[0])));

    // Stepping with + walks - > C ready > C > B ready > B > A ready > A (> S ready > S)
    const char *want_slow[] = {"-", "C ready", "C", "B ready", "B", "A ready", "A", "S ready", "S"};
    const char *want_friend[] = {"-", "C ready", "C", "B ready", "B", "A ready", "A"};
    for (int t = 0; t < 2; t++) {
        int type = t ? 1 : 0, v = 0, n = 0;
        const char **want = t ? want_slow : want_friend;
        int count = t ? 9 : 7;
        char got[16];
        for (;;) {
            fe_support_rank_name(type, v, got, sizeof(got));
            CHECK(n < count && strcmp(got, want[n]) == 0, "type %d step %d: '%s'", type, n, got);
            n++;
            int nx = fe_support_next_step(type, v);
            if (nx < 0) break;
            CHECK(fe_support_prev_step(type, nx) == v, "type %d: prev of %d is %d, want %d", type, nx, fe_support_prev_step(type, nx), v);
            v = nx;
        }
        CHECK(n == count, "type %d: %d steps, want %d", type, n, count);
    }
    printf("  step sequences ok (friendship 7 stages, romance 9 stages)\n");
}

// Marriage guard: marry A to B, give A and B other "S ready" pairs plus an unrelated
// S-ready pair, then check only the married ones drop to A rank.
static void test_marriage_guard(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }

    // Find unit A with 2+ romantic partners present in the save
    int ai = -1, p1 = -1, p2 = -1, t1 = 0, t2 = 0;
    for (int i = 0; i < s->unit_count && ai < 0; i++) {
        uint16_t id = fe_unit_char_id(s, &s->units[i]);
        if (id >= FE_CHAR_COUNT || fe_find_unit(s, id) != i) continue;
        int found = 0;
        for (int k = 0; k < fe_supports[id].count && found < 2; k++) {
            const fe_support_def *d = &fe_supports[id].partners[k];
            if (d->type == 0 || fe_find_unit(s, d->partner) < 0) continue;
            if (!found) { p1 = d->partner; t1 = d->type; } else { p2 = d->partner; t2 = d->type; }
            found++;
        }
        if (found == 2) ai = i;
    }
    if (ai < 0) { printf("  marriage guard: no unit with 2 romantic partners present, skipped\n"); goto done; }

    static const int SR[4] = {-1, 21, 19, 17}, AR[4] = {-1, 16, 14, 13};
    int achar = fe_unit_char_id(s, &s->units[ai]);
    CHECK(fe_support_set(s, ai, p1, SR[t1] + 1, err, sizeof(err)), "marry: %s", err);       // A married to p1
    CHECK(fe_support_set(s, ai, p2, SR[t2], err, sizeof(err)), "S ready p2: %s", err);       // trap
    CHECK(fe_support_spouse(s, ai, -1) == p1, "spouse not detected");
    CHECK(fe_support_s_would_unmarry(s, ai, p2), "S ready with p2 should be flagged");
    CHECK(!fe_support_s_would_unmarry(s, ai, p1) || fe_support_spouse(s, fe_find_unit(s, p1), achar) >= 0,
          "own spouse pair should not be flagged unless the spouse is married elsewhere");

    char report[512];
    int n = fe_support_fix_pending_s(s, report, sizeof(report));
    CHECK(n >= 1, "no pairs fixed");
    CHECK(fe_support_get(s, &s->units[ai], p1) == SR[t1] + 1, "marriage was changed");
    CHECK(fe_support_get(s, &s->units[ai], p2) == AR[t2], "trap pair is %d, want A rank %d", fe_support_get(s, &s->units[ai], p2), AR[t2]);
    int p2i = fe_find_unit(s, p2);
    CHECK(fe_support_get(s, &s->units[p2i], achar) == AR[t2], "partner side not lowered");
    CHECK(fe_support_fix_pending_s(s, NULL, 0) == 0, "second pass should change nothing");
    printf("  marriage guard ok: %s", report);
done:
    fe_save_free(s);
    free(s);
    free(file);
}

// Inventory and convoy: packing, equip-to-top, convoy counts, nothing else touched.
static void test_items(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    CHECK(s->convoy_count == FE_ITEM_COUNT + 150, "convoy table has %d entries, want 352", s->convoy_count);

    const fe_unit *u = NULL;
    for (int i = 0; i < s->unit_count && !u; i++)
        if (s->units[i].group == FE_GROUP_ARMY || s->units[i].group == FE_GROUP_BLUE) u = &s->units[i];
    if (!u) goto done;

    uint8_t *orig = malloc(s->size);
    memcpy(orig, s->data, s->size);
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    // Fill all 5 slots, remove the middle one, check packing, equip the last, check it moved to top
    for (int k = 0; k < FE_INV_SLOTS; k++) fe_inv_set(s, u, k, 1 + k, 10 + k);
    fe_inv_set(s, u, 2, 0, 0);
    fe_inv_item it[FE_INV_SLOTS];
    for (int k = 0; k < FE_INV_SLOTS; k++) fe_inv_get(s, u, k, &it[k]);
    CHECK(it[0].id == 1 && it[1].id == 2 && it[2].id == 4 && it[3].id == 5 && it[4].id == 0, "inventory not packed after remove");
    fe_inv_equip(s, u, 3);
    for (int k = 0; k < FE_INV_SLOTS; k++) fe_inv_get(s, u, k, &it[k]);
    CHECK(it[0].id == 5 && (it[0].flags & FE_ITEM_EQUIPPED), "equipped item not moved to top");
    int eq = 0;
    for (int k = 0; k < FE_INV_SLOTS; k++) eq += (it[k].flags & FE_ITEM_EQUIPPED) != 0;
    CHECK(eq == 1, "%d slots marked equipped", eq);
    fe_inv_set(s, u, 0, 1, 999);
    fe_inv_get(s, u, 0, &it[0]);
    CHECK(it[0].uses == 255, "uses not clamped to 255");

    // Convoy: 3 Bronze Swords (50 uses each), 7 Vulneraries, unbreakable counts
    fe_convoy_set_items(s, 1, 3);
    CHECK(fe_convoy_get(s, 1) == 150 && fe_convoy_count_items(s, 1) == 3, "convoy bronze sword");
    fe_convoy_set_items(s, 152, 7);
    CHECK(fe_convoy_count_items(s, 152) == 7, "convoy vulnerary");
    fe_convoy_set(s, 1, 100000);
    CHECK(fe_convoy_get(s, 1) == 0xFFFF, "convoy uses not clamped");
    fe_convoy_set_items(s, 1, 3);

    size_t stray = 0;
    for (uint32_t i = 0; i < s->size; i++) {
        bool ok = (i >= u->off + 0x1A && i < u->off + 0x1A + 25) ||
                  (i >= s->tran_off + 7 && i < s->tran_off + 7 + (uint32_t)s->convoy_count * 2);
        if (s->data[i] != orig[i] && !ok) stray++;
    }
    CHECK(stray == 0, "%u bytes changed outside inventory/convoy", (unsigned)stray);

    uint8_t *out; size_t out_len;
    if (fe_save_build(s, &out, &out_len, err, sizeof(err))) {
        fe_save *r = malloc(sizeof(fe_save));
        if (fe_save_load(r, out, out_len, err, sizeof(err))) {
            CHECK(memcmp(r->data, s->data, s->size) == 0, "reloaded save differs after item edits");
            fe_save_free(r);
        } else CHECK(false, "reload after item edit: %s", err);
        free(r);
        free(out);
    } else CHECK(false, "build after item edit: %s", err);

    char fname[32] = "";
    if (s->forge_count) {
        fe_forge_name(s, 0, fname, sizeof(fname));
    }
    printf("  items ok on %s: pack/equip/clamp, convoy counts, round trip%s%s\n", name,
           s->forge_count ? "; first forge: " : "", fname);
    free(orig);
done:
    fe_save_free(s);
    free(s);
    free(file);
}

// Avatar look + rename (log trailer and slot header), nothing else touched.
static void test_avatar(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    const fe_unit *u = NULL;
    for (int i = 0; i < s->unit_count && !u; i++) {
        uint16_t id = fe_unit_char_id(s, &s->units[i]);
        if ((id == 0 || id == 1) && s->units[i].log_off >= 0 &&
            (s->units[i].group == FE_GROUP_ARMY || s->units[i].group == FE_GROUP_BLUE))
            u = &s->units[i];
    }
    if (!u) goto done;
    uint8_t *orig = malloc(s->size);
    memcpy(orig, s->data, s->size);
    char before[48];
    fe_unit_name(s, u, before, sizeof(before));
    bool header_matched = memcmp(s->data + 0x10, s->data + u->log_off, 0x1A) == 0;

    // Every hero face id has a name, and Pr. Marth's preset matches the real DLC unit's save data
    for (int i = 0; i < FE_HERO_FACE_COUNT; i++)
        CHECK(fe_hero_face_name(fe_hero_faces[i]) != NULL, "hero face %d has no name", fe_hero_faces[i]);
    CHECK(fe_hero_face_name(11) && strcmp(fe_hero_face_name(11), "Pr. Marth") == 0, "face 11 is not Marth");
    for (int i = 0; i < FE_LOOK_PRESET_COUNT; i++)
        if (strcmp(fe_look_presets[i].name, "Pr. Marth") == 0 && fe_look_presets[i].hero_head) {
            fe_look_apply_preset(s, u, &fe_look_presets[i]);
            // values read from the real "Pr. Marth" DLC unit in the tester's save
            CHECK(fe_look_get(s, u, FE_LOOK_FACE) == 11 && fe_look_get(s, u, FE_LOOK_HAIR) == 0 &&
                  fe_look_get(s, u, FE_LOOK_BUILD) == 1 && fe_look_get(s, u, FE_LOOK_VOICE) == 4 &&
                  fe_look_color(s, u) == 0x325AB4, "Marth preset differs from the real DLC unit");
        }
    // Hair color goes to both copies (logbook and main record AI block +0x39)
    fe_look_set_color(s, u, 0x123456);
    {
        const uint8_t *c2 = s->data + u->flags_off + 0x2A + 0x0D + 0x39;
        CHECK(c2[0] == 0x12 && c2[1] == 0x34 && c2[2] == 0x56 && c2[3] == 0xFF, "main-record hair color not written");
    }
    // Hero identity: matches the real Pr. Marth unit (id byte 0xC9, flag bit 1) and restores
    {
        uint8_t own[FE_LOG_ID_SIZE], back[FE_LOG_ID_SIZE];
        fe_log_id_get(s, u, own);
        const fe_look_preset *marth = NULL;
        for (int i = 0; i < FE_LOOK_PRESET_COUNT; i++)
            if (fe_look_presets[i].hero_head && fe_look_presets[i].face == 11) marth = &fe_look_presets[i];
        CHECK(marth && marth->log_id == 0xC9, "Marth preset log id is not 0xC9");
        if (marth) {
            fe_log_set_hero_identity(s, u, marth);
            CHECK(s->data[u->log_off + 0x27] == 0xC9 && (s->data[u->log_off + 0x37] & 2), "hero identity bytes");
            CHECK(fe_log_hero_identity(s, u) == marth, "hero identity not detected");
            fe_log_id_set(s, u, own);
            fe_log_set_hero_flag(s, u, false);
            fe_log_id_get(s, u, back);
            CHECK(memcmp(own, back, FE_LOG_ID_SIZE) == 0 && !fe_log_hero_identity(s, u), "identity not restored");
        }
    }
    fe_look_set(s, u, FE_LOOK_FACE, 11);  // Marth's hero face
    fe_look_set(s, u, FE_LOOK_HAIR, 3);
    fe_look_set_color(s, u, 0x325AB4);
    CHECK(fe_look_get(s, u, FE_LOOK_FACE) == 11 && fe_look_color(s, u) == 0x325AB4, "look fields");
    int n = fe_unit_set_name(s, u, "Grima-Robin12345");
    CHECK(n == FE_NAME_MAX, "name not truncated to %d (got %d)", FE_NAME_MAX, n);
    char after[48];
    fe_unit_name(s, u, after, sizeof(after));
    CHECK(strcmp(after, "Grima-Robin1") == 0, "name reads back as '%s'", after);
    if (header_matched)
        CHECK(memcmp(s->data + 0x10, s->data + u->log_off, 0x1A) == 0, "slot header name not updated");

    size_t stray = 0;
    for (uint32_t i = 0; i < s->size; i++) {
        bool ok = (i >= (uint32_t)u->log_off && i < (uint32_t)u->log_off + 0x27) || (i >= 0x10 && i < 0x2A) ||
                  (i >= u->flags_off + 0x2A + 0x0D + 0x39 && i < u->flags_off + 0x2A + 0x0D + 0x3D);
        if (s->data[i] != orig[i] && !ok) stray++;
    }
    CHECK(stray == 0, "%u bytes changed outside the logbook look/name and header name", (unsigned)stray);

    uint8_t *out; size_t out_len;
    if (fe_save_build(s, &out, &out_len, err, sizeof(err))) {
        fe_save *r = malloc(sizeof(fe_save));
        if (fe_save_load(r, out, out_len, err, sizeof(err))) {
            CHECK(memcmp(r->data, s->data, s->size) == 0, "reloaded save differs after avatar edit");
            fe_save_free(r);
        } else CHECK(false, "reload after avatar edit: %s", err);
        free(r);
        free(out);
    } else CHECK(false, "build after avatar edit: %s", err);
    printf("  avatar ok: '%s' -> '%s', face 11, hair color, header %s\n", before, after,
           header_matched ? "renamed too" : "left alone (did not match)");
    free(orig);
done:
    fe_save_free(s);
    free(s);
    free(file);
}

// Global file: support log / gallery completion and the Game Clear flag
static void test_global(const char *path)
{
    printf("=== %s\n", path);
    size_t len;
    uint8_t *file = read_file(path, &len);
    if (!file) { printf("  (no Global file)\n"); return; }
    fe_global g;
    char err[160];
    if (!fe_global_load(&g, file, len, err, sizeof(err))) { CHECK(false, "global load: %s", err); free(file); return; }
    printf("  game clear %d, support log %d/%d, gallery %d/%d (lists: %u/%u/%u bits)\n", fe_global_game_clear(&g),
           fe_global_support_count(&g), FE_SUPPORT_LOG_ENTRIES, fe_global_gallery_count(&g), FE_GALLERY_ENTRIES,
           (unsigned)g.gallery_bits, (unsigned)g.support_bits, (unsigned)g.colors_bits);

    char mname[32], fname[32];
    fe_global_avatar_name(&g, false, mname, sizeof(mname));
    fe_global_avatar_name(&g, true, fname, sizeof(fname));
    printf("  scene Avatars: male '%s', female '%s'%s\n", mname, fname, fe_global_avatar_empty(&g, true) ? " (empty)" : "");

    uint8_t *orig = malloc(g.size);
    memcpy(orig, g.data, g.size);
    uint8_t *out; size_t out_len;
    CHECK(fe_global_build(&g, &out, &out_len, err, sizeof(err)), "unedited global build: %s", err);

    // Fill the female profile from a logbook block (here: the male one, renamed), then revert
    {
        uint8_t lb[0x188];
        memcpy(lb, g.data + g.avatar_off + 1, FE_GLOBAL_AVATAR_SIZE);
        memset(lb, 0, 0x1A);
        lb[0] = 'R'; lb[2] = 'o'; lb[4] = 'b'; lb[6] = 'i'; lb[8] = 'n'; lb[10] = 'F';
        lb[0x1C] = 1;  // female
        fe_global_set_avatar(&g, true, lb);
        fe_global_avatar_name(&g, true, fname, sizeof(fname));
        CHECK(strcmp(fname, "RobinF") == 0 && !fe_global_avatar_empty(&g, true), "female profile not set");
        size_t stray = 0;
        uint32_t f0 = g.avatar_off + 1 + FE_GLOBAL_AVATAR_SIZE + 1;
        for (uint32_t i = 0; i < g.size; i++)
            if (g.data[i] != orig[i] && !(i >= f0 && i < f0 + FE_GLOBAL_AVATAR_SIZE)) stray++;
        CHECK(stray == 0, "%u bytes changed outside the female profile", (unsigned)stray);
        uint8_t *fout = NULL;
        size_t fout_len = 0;
        CHECK(fe_global_build(&g, &fout, &fout_len, err, sizeof(err)), "build with female profile: %s", err);
        free(fout);
        printf("  scene Avatar ok: female profile set to '%s', nothing else changed, round trip\n", fname);
        memcpy(g.data, orig, g.size);
    }
    free(out);

    // Undo: snapshot the raw data, complete, then copy the snapshot's logs back
    {
        fe_global snap, cur;
        CHECK(fe_global_load_raw(&snap, g.data, g.size, err, sizeof(err)), "snapshot load: %s", err);
        CHECK(fe_global_load_raw(&cur, g.data, g.size, err, sizeof(err)), "copy load: %s", err);
        fe_global_complete_logs(&cur);
        fe_global_set_game_clear(&cur, true);
        CHECK(fe_global_is_complete(&cur), "not complete after complete");
        CHECK(fe_global_copy_logs(&cur, &snap, err, sizeof(err)), "copy logs: %s", err);
        CHECK(cur.size == g.size && memcmp(cur.data, g.data, g.size) == 0, "undo did not restore the original bytes");
        printf("  undo ok: complete then restore returns the original file byte for byte\n");
        fe_global_free(&snap);
        fe_global_free(&cur);
    }
    fe_global_complete_logs(&g);
    fe_global_set_game_clear(&g, true);
    CHECK(fe_global_support_count(&g) == FE_SUPPORT_LOG_ENTRIES && fe_global_gallery_count(&g) == FE_GALLERY_ENTRIES &&
          fe_global_game_clear(&g), "complete/game clear not applied");
    size_t stray = 0;
    for (uint32_t i = 0; i < g.size; i++) {
        bool ok = i == g.flags_off ||
                  (i >= g.gallery_off && i < g.gallery_off + g.gallery_bits / 8 + 1) ||
                  (i >= g.support_off && i < g.support_off + g.support_bits / 8 + 1) ||
                  (i >= g.colors_off && i < g.colors_off + g.colors_bits / 8 + 1);
        if (g.data[i] != orig[i] && !ok) stray++;
    }
    CHECK(stray == 0, "%u global bytes changed outside the flag lists", (unsigned)stray);
    if (fe_global_build(&g, &out, &out_len, err, sizeof(err))) {
        fe_global r;
        CHECK(fe_global_load(&r, out, out_len, err, sizeof(err)) && r.size == g.size &&
              memcmp(r.data, g.data, g.size) == 0, "global reload differs");
        fe_global_free(&r);
        free(out);
        printf("  complete ok: support log %d/%d, gallery %d/%d, game clear on, round trip\n",
               FE_SUPPORT_LOG_ENTRIES, FE_SUPPORT_LOG_ENTRIES, FE_GALLERY_ENTRIES, FE_GALLERY_ENTRIES);
    } else CHECK(false, "global build after complete: %s", err);
    free(orig);
    fe_global_free(&g);
    free(file);
}

// Adding units: second Avatar, then each Robin's Morgan with the right parents.
static void test_add_units(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    int main_av = fe_find_unit(s, 0) >= 0 ? fe_find_unit(s, 0) : fe_find_unit(s, 1);
    if (main_av < 0) { printf("  add units: no Avatar in this save, skipped\n"); goto done; }
    int main_char = fe_unit_char_id(s, &s->units[main_av]);
    int other_av = main_char == 0 ? 1 : 0;
    if (fe_find_unit(s, other_av) >= 0) { printf("  add units: both Avatars already present, skipped\n"); goto done; }

    // Snapshot the records and the tail for the integrity check
    int n0 = s->unit_count;
    uint32_t tail_len = s->size - s->unit_end;
    uint8_t *tail = malloc(tail_len);
    memcpy(tail, s->data + s->unit_end, tail_len);
    uint32_t gold = fe_get_gold(s);

    int ni = fe_unit_add(s, main_av, other_av, err, sizeof(err));
    CHECK(ni >= 0, "add Avatar: %s", err);
    if (ni < 0) { free(tail); goto done; }
    const fe_unit *u = &s->units[ni];
    fe_stats st;
    CHECK(s->unit_count == n0 + 1, "unit count %d, want %d", s->unit_count, n0 + 1);
    CHECK(fe_unit_char_id(s, u) == other_av && u->group == FE_GROUP_ARMY, "new unit identity");
    CHECK(fe_unit_class_id(s, u) == fe_recruit[other_av].start_class && fe_unit_level(s, u) == 1, "class/level");
    CHECK(u->log_off < 0 && u->child_off < 0 && s->data[u->off + 0x43] == 0, "trailers/supports not cleared");
    CHECK(fe_unit_stats(s, u, &st) && s->data[u->off + 0x14] == fe_stat_shown(&st, 0), "new unit not at full HP");
    CHECK(memcmp(s->data + s->unit_end, tail, tail_len) == 0, "blocks after UNIT changed");
    CHECK(fe_get_gold(s) == gold, "gold changed");
    free(tail);
    printf("  added %s: %s Lv1, HP %d\n", fe_chars[other_av].name, fe_class_name(fe_unit_class_id(s, u)), fe_stat_shown(&st, 0));

    // Give the new Avatar logbook data so the game shows a name and portrait
    {
        int units_before = s->unit_count;
        uint32_t tail2_len = s->size - s->unit_end;
        uint8_t *tail2 = malloc(tail2_len);
        memcpy(tail2, s->data + s->unit_end, tail2_len);
        CHECK(fe_unit_add_logbook(s, ni, main_av, err, sizeof(err)), "add logbook: %s", err);
        u = &s->units[ni];
        char nm[32];
        fe_unit_name(s, u, nm, sizeof(nm));
        uint8_t own[FE_LOG_ID_SIZE], mainid[FE_LOG_ID_SIZE];
        fe_log_id_get(s, u, own);
        fe_log_id_get(s, &s->units[main_av], mainid);
        CHECK(u->log_off >= 0 && strcmp(nm, "Robin") == 0, "logbook name '%s'", nm);
        CHECK(fe_look_get(s, u, FE_LOOK_GENDER) == (other_av == 1), "logbook gender");
        CHECK(memcmp(own, mainid, FE_LOG_ID_SIZE) != 0, "new Avatar shares the main Avatar's logbook id");
        CHECK(s->unit_count == units_before && memcmp(s->data + s->unit_end, tail2, tail2_len) == 0,
              "logbook insert disturbed other data");
        CHECK(fe_unit_stats(s, u, &st), "stats after logbook");
        s->data[u->off + 0x14] = (uint8_t)fe_stat_shown(&st, 0);
        printf("  gave %s logbook data: name Robin, %s, own logbook id, HP %d\n", fe_chars[other_av].name,
               other_av == 1 ? "female" : "male", fe_stat_shown(&st, 0));
        free(tail2);
    }

    // Marry the new Avatar (F or M) to someone present, then add their Morgan
    int avf = fe_find_unit(s, 1);
    int spouse = -1;
    for (int k = 0; k < fe_supports[1].count && spouse < 0; k++) {
        const fe_support_def *d = &fe_supports[1].partners[k];
        if (d->type && fe_find_unit(s, d->partner) >= 0 && d->partner != 0) spouse = d->partner;
    }
    if (avf >= 0 && spouse >= 0) {
        static const int SR[4] = {-1, 21, 19, 17};
        int type = 0;
        for (int k = 0; k < fe_supports[1].count; k++)
            if (fe_supports[1].partners[k].partner == spouse) type = fe_supports[1].partners[k].type;
        CHECK(fe_support_set(s, avf, spouse, SR[type] + 1, err, sizeof(err)), "marry Avatar (F): %s", err);
    }
    int mi = fe_unit_add(s, main_av, 39, err, sizeof(err));  // Morgan (M), fixed parent Avatar (F)
    CHECK(mi >= 0, "add Morgan (M): %s", err);
    if (mi >= 0) {
        const uint8_t *c = s->data + s->units[mi].child_off;
        int father = c[3] | (c[4] << 8), mother = c[20] | (c[21] << 8);
        CHECK(s->units[mi].child_off >= 0, "Morgan (M) has no child block");
        CHECK(mother == 1, "Morgan (M) mother is %d, want Avatar (F)", mother);
        if (spouse >= 0) CHECK(father == spouse, "Morgan (M) father is %d, want spouse %d", father, spouse);
        CHECK(fe_unit_stats(s, &s->units[mi], &st), "Morgan (M) stats");
        printf("  added Morgan (M): mother Avatar (F), father %s\n", father < FE_CHAR_COUNT ? fe_chars[father].name : "(none)");
    }
    int fi = fe_unit_add(s, main_av, 40, err, sizeof(err));  // Morgan (F), fixed parent Avatar (M)
    CHECK(fi >= 0, "add Morgan (F): %s", err);
    if (fi >= 0) {
        const uint8_t *c = s->data + s->units[fi].child_off;
        CHECK((c[3] | (c[4] << 8)) == 0, "Morgan (F) father is not Avatar (M)");
        printf("  added Morgan (F): father Avatar (M), mother %s\n",
               (c[20] | (c[21] << 8)) < FE_CHAR_COUNT ? fe_chars[c[20] | (c[21] << 8)].name : "(none: Avatar (M) unmarried)");
    }

    uint8_t *out; size_t out_len;
    if (fe_save_build(s, &out, &out_len, err, sizeof(err))) {
        fe_save *r = malloc(sizeof(fe_save));
        if (fe_save_load(r, out, out_len, err, sizeof(err))) {
            CHECK(r->unit_count == n0 + 3 && memcmp(r->data, s->data, s->size) == 0, "reload after adding units");
            fe_save_free(r);
        } else CHECK(false, "reload after adding units: %s", err);
        free(r);
        free(out);
    } else CHECK(false, "build after adding units: %s", err);
done:
    fe_save_free(s);
    free(s);
    free(file);
}

static uint32_t rng_state = 12345;
static uint32_t rng(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

// Weapon ranks, Boots, records, heal, revive, remove
static void test_unit_tools(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }

    // Weapon ranks
    CHECK(!strcmp(fe_wexp_rank(0), "E") && !strcmp(fe_wexp_rank(14), "E") && !strcmp(fe_wexp_rank(15), "D") &&
          !strcmp(fe_wexp_rank(35), "C") && !strcmp(fe_wexp_rank(60), "B") && !strcmp(fe_wexp_rank(90), "A"),
          "rank thresholds");
    CHECK(fe_wexp_next_rank(0) == 15 && fe_wexp_next_rank(15) == 35 && fe_wexp_next_rank(89) == 90 &&
          fe_wexp_next_rank(90) == 90 && fe_wexp_prev_rank(90) == 60 && fe_wexp_prev_rank(16) == 15 &&
          fe_wexp_prev_rank(0) == 0, "rank steps");
    int ai = -1;
    for (int i = 0; i < s->unit_count && ai < 0; i++)
        if (s->units[i].group == FE_GROUP_ARMY) ai = i;
    if (ai >= 0) {
        uint8_t *before = malloc(s->size);
        memcpy(before, s->data, s->size);
        const fe_unit *u = &s->units[ai];
        fe_unit_set_wexp(s, u, 2, 200);
        CHECK(fe_unit_wexp(s, u, 2) == FE_WEXP_MAX, "weapon exp not clamped to 90");
        fe_unit_set_boots(s, u, 2);
        fe_unit_set_record(s, u, false, 1234);
        fe_unit_set_record(s, u, true, 70000);
        CHECK(fe_unit_boots(s, u) == 2 && fe_unit_record(s, u, false) == 1234 && fe_unit_record(s, u, true) == 65535,
              "boots/records");
        size_t changed = 0;
        for (uint32_t i = 0; i < s->size; i++) changed += s->data[i] != before[i];
        CHECK(changed <= 6, "%u bytes changed for 4 small edits", (unsigned)changed);
        memcpy(s->data, before, s->size);
        free(before);
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        printf("  unit tools ok on %s: weapon exp %d %d %d %d %d %d, battles %d, victories %d\n", name,
               fe_unit_wexp(s, u, 0), fe_unit_wexp(s, u, 1), fe_unit_wexp(s, u, 2), fe_unit_wexp(s, u, 3),
               fe_unit_wexp(s, u, 4), fe_unit_wexp(s, u, 5), fe_unit_record(s, u, false), fe_unit_record(s, u, true));
    }

    // Revive every fallen unit
    int dead = 0, n0 = s->unit_count;
    uint32_t tail_len = s->size - s->unit_end;
    uint8_t *tail = malloc(tail_len);
    memcpy(tail, s->data + s->unit_end, tail_len);
    for (;;) {
        int di = -1;
        for (int i = 0; i < s->unit_count; i++)
            if (s->units[i].group == FE_GROUP_DEAD) { di = i; break; }
        if (di < 0) break;
        char name[48];
        fe_unit_name(s, &s->units[di], name, sizeof(name));
        uint16_t cid = fe_unit_char_id(s, &s->units[di]);
        bool ok = fe_unit_revive(s, di, err, sizeof(err));
        CHECK(ok, "revive %s: %s", name, err);
        if (!ok) break;
        int ni = -1;
        for (int i = 0; i < s->unit_count; i++)
            if (s->units[i].group == FE_GROUP_ARMY && fe_unit_char_id(s, &s->units[i]) == cid) ni = i;
        CHECK(ni >= 0, "%s not in the army after revive", name);
        if (ni >= 0) {
            const fe_unit *u = &s->units[ni];
            fe_stats st;
            CHECK((s->data[u->flags_off + 0x0F] & 0x88) == 0, "%s still flagged fallen", name);
            CHECK(!fe_unit_stats(s, u, &st) || s->data[u->off + 0x14] == fe_stat_shown(&st, 0), "%s not at full HP", name);
        }
        printf("  revived %s\n", name);
        dead++;
    }
    if (dead) {
        CHECK(s->unit_count == n0, "unit count changed by revive");
        CHECK(memcmp(s->data + s->unit_end, tail, tail_len) == 0, "blocks after UNIT changed by revive");
        uint8_t *out; size_t out_len;
        CHECK(fe_save_build(s, &out, &out_len, err, sizeof(err)), "build after revive: %s", err);
        if (out) free(out);
    }

    // Remove the last army unit, then check the rest is intact
    int last = -1;
    for (int i = 0; i < s->unit_count; i++)
        if (s->units[i].group == FE_GROUP_ARMY) last = i;
    if (last >= 0) {
        int n1 = s->unit_count;
        uint32_t removed = s->units[last].size;
        uint32_t size1 = s->size;
        bool ok = fe_unit_remove(s, last, err, sizeof(err));
        CHECK(ok, "remove: %s", err);
        if (ok) {
            CHECK(s->unit_count == n1 - 1 && s->size == size1 - removed, "remove: count/size");
            CHECK(memcmp(s->data + s->unit_end, tail, tail_len) == 0, "blocks after UNIT changed by remove");
            uint8_t *out; size_t out_len;
            CHECK(fe_save_build(s, &out, &out_len, err, sizeof(err)), "build after remove: %s", err);
            if (out) free(out);
            printf("  removed the last army unit (%u bytes), round trip\n", (unsigned)removed);
        }
    }
    free(tail);
    fe_save_free(s);
    free(s);
    free(file);
}

// Import Avatars from every given save (and the Global logbook) into each save
static char g_import_paths[16][512];
static int g_import_count = 0;
static char g_global_path[512] = "";

static void import_one(fe_save *dst, const fe_import_cand *c, const char *dst_name)
{
    char err[256];
    fe_save *t = malloc(sizeof(fe_save));
    if (!fe_save_clone(dst, t)) { free(t); return; }
    int n0 = t->unit_count;
    uint32_t tail_len = t->size - t->unit_end;
    uint8_t *tail = malloc(tail_len);
    memcpy(tail, t->data + t->unit_end, tail_len);
    int ni = fe_import_add(t, c, true, err, sizeof(err));
    CHECK(ni >= 0, "import %s (%s) into %s: %s", c->name, c->source, dst_name, err);
    if (ni >= 0) {
        const fe_unit *u = &t->units[ni];
        char name[48];
        fe_unit_name(t, u, name, sizeof(name));
        uint16_t cid = fe_unit_char_id(t, u);
        fe_stats st;
        bool hs = fe_unit_stats(t, u, &st);
        CHECK(t->unit_count == n0 + 1 && u->group == FE_GROUP_ARMY, "import %s: count/group", c->name);
        CHECK(cid == 0 || cid == 1, "import %s: char id %u", c->name, cid);
        CHECK(!strcmp(name, c->name), "import %s: name '%s'", c->name, name);
        CHECK(u->log_off >= 0 && u->child_off < 0, "import %s: trailers", c->name);
        CHECK(memcmp(t->data + t->unit_end, tail, tail_len) == 0, "import %s: blocks after UNIT changed", c->name);
        uint8_t *out = NULL; size_t out_len;
        CHECK(fe_save_build(t, &out, &out_len, err, sizeof(err)), "import %s: build: %s", c->name, err);
        free(out);
        int ns = 0, over = 0;
        if (cid < FE_CHAR_COUNT)
            for (int k = 0; k < fe_supports[cid].count; k++) {
                int v = fe_support_get(t, u, fe_supports[cid].partners[k].partner);
                if (v > 0) ns++;
                if (v > fe_support_a_rank(fe_supports[cid].partners[k].type)) over++;
            }
        CHECK(over == 0, "import %s: %d supports above A", c->name, over);
        if (ns) printf("    (%s keeps %d support%s, all A or lower)\n", c->name, ns, ns == 1 ? "" : "s");
        char it[32];
        fe_inv_item inv;
        fe_inv_get(t, u, 0, &inv);
        fe_item_name(t, inv.id, it, sizeof(it));
        printf("    %-10s from %-34s -> %s %s Lv%d, HP %d Str %d Mag %d, item1 %s\n", c->name, c->source,
               cid ? "(F)" : "(M)", fe_class_name(fe_unit_class_id(t, u)), fe_unit_level(t, u),
               hs ? fe_stat_shown(&st, 0) : -1, hs ? fe_stat_shown(&st, 1) : -1, hs ? fe_stat_shown(&st, 2) : -1,
               inv.id ? it : "-");
    }
    free(tail);
    fe_save_free(t);
    free(t);
}

static void test_import(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *dst = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(dst, file, len, err, sizeof(err))) { free(dst); free(file); return; }
    fe_import_cand *c = malloc(sizeof(fe_import_cand) * 64);
    int total = 0;
    for (int k = 0; k < g_import_count; k++) {
        size_t sl;
        uint8_t *sf = read_file(g_import_paths[k], &sl);
        fe_save *src = malloc(sizeof(fe_save));
        if (sf && fe_save_load(src, sf, sl, err, sizeof(err))) {
            const char *base = strrchr(g_import_paths[k], '/');
            int n = fe_import_gather_save(src, base ? base + 1 : g_import_paths[k], false, c, 64);
            for (int i = 0; i < n; i++) import_one(dst, &c[i], path);
            total += n;
            fe_save_free(src);
        }
        free(src);
        free(sf);
    }
    if (g_global_path[0]) {
        size_t gl;
        uint8_t *gf = read_file(g_global_path, &gl);
        fe_global g;
        if (gf && fe_global_load(&g, gf, gl, err, sizeof(err))) {
            int n = fe_import_gather_logbook(g.data, g.size, c, 64);
            for (int i = 0; i < n; i++) import_one(dst, &c[i], path);
            total += n;
            fe_global_free(&g);
        }
        free(gf);
    }
    {   // the hero list, with and without supports
        fe_import_cand *all = malloc(sizeof(fe_import_cand) * 160);
        int n = fe_import_gather_heroes(all, 160), ok = 0;
        for (int i = 0; i < n; i++) {
            for (int sup = 0; sup < 2; sup++) {
                fe_save *t = malloc(sizeof(fe_save));
                if (!fe_save_clone(dst, t)) { free(t); continue; }
                int ni = fe_import_add(t, &all[i], sup == 1, err, sizeof(err));
                CHECK(ni >= 0, "hero %s (supports %d): %s", all[i].name, sup, err);
                if (ni >= 0) {
                    const fe_unit *u = &t->units[ni];
                    int cid = fe_unit_char_id(t, u);
                    char name[48];
                    fe_unit_name(t, u, name, sizeof(name));
                    CHECK(!strcmp(name, all[i].name), "hero name '%s' vs '%s'", name, all[i].name);
                    CHECK(sup ? (cid == 0 || cid == 1) : (cid != 0 && cid != 1), "hero %s char %d (supports %d)", name, cid, sup);
                    uint8_t *out = NULL; size_t ol;
                    CHECK(fe_save_build(t, &out, &ol, err, sizeof(err)), "hero %s build: %s", name, err);
                    free(out);
                    if (sup && (i == 0 || !strcmp(all[i].name, "Ike") || !strcmp(all[i].name, "Lyn"))) {
                        fe_stats st;
                        bool hs = fe_unit_stats(t, u, &st);
                        printf("    hero %-6s -> %s Lv%d HP %d Str %d Spd %d (%s)\n", name, fe_class_name(fe_unit_class_id(t, u)),
                               fe_unit_level(t, u), hs ? fe_stat_shown(&st, 0) : -1, hs ? fe_stat_shown(&st, 1) : -1,
                               hs ? fe_stat_shown(&st, 4) : -1, all[i].source);
                    }
                    ok++;
                }
                fe_save_free(t);
                free(t);
            }
        }
        free(all);
        printf("  hero list ok: %d imports (%d heroes, supports on and off)\n", ok, n);
    }
    printf("  import ok: %d Avatar candidates imported into this save\n", total);
    free(c);
    fe_save_free(dst);
    free(dst);
    free(file);
}

// Difficulty / mode and changing a child's parents
static void test_difficulty_parents(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    static const char *const dn[3] = {"Normal", "Hard", "Lunatic"};
    int d0 = fe_difficulty(s);
    bool c0 = fe_is_casual(s), l0 = fe_is_lunatic_plus(s);
    CHECK(d0 >= 0 && d0 <= 2 && s->data[0x0D] == d0, "difficulty %d / header %d", d0, s->data[0x0D]);
    CHECK(c0 == ((s->data[0x08] & 0x04) != 0), "casual flag differs between header and USER");
    uint8_t *before = malloc(s->size);
    memcpy(before, s->data, s->size);
    fe_set_difficulty(s, 2);
    fe_set_lunatic_plus(s, true);
    fe_set_casual(s, !c0);
    CHECK(fe_difficulty(s) == 2 && fe_is_lunatic_plus(s) && fe_is_casual(s) == !c0, "difficulty setters");
    size_t changed = 0;
    for (uint32_t i = 0; i < s->size; i++) changed += s->data[i] != before[i];
    CHECK(changed <= 6, "difficulty edit changed %u bytes", (unsigned)changed);
    uint8_t *out = NULL; size_t out_len;
    CHECK(fe_save_build(s, &out, &out_len, err, sizeof(err)), "build after difficulty edit: %s", err);
    free(out);
    memcpy(s->data, before, s->size);
    free(before);
    printf("  difficulty ok: %s%s, %s (setters write header and USER copies)\n", dn[d0], l0 ? "+" : "",
           c0 ? "Casual" : "Classic");

    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        if (u->child_off < 0 || u->group == FE_GROUP_RED) continue;
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        int f0 = fe_child_parent(s, u, 0), m0 = fe_child_parent(s, u, 1);
        fe_stats a, b;
        bool ha = fe_unit_stats(s, u, &a);
        fe_child_set_parent(s, u, 0, 3);   // Chrom
        fe_child_set_parent(s, u, 1, 12);  // Sumia
        CHECK(fe_child_parent(s, u, 0) == 3 && fe_child_parent(s, u, 1) == 12, "%s: parents not set", name);
        bool hb = fe_unit_stats(s, u, &b);
        fe_child_set_parent(s, u, 0, -1);
        CHECK(fe_child_parent(s, u, 0) == -1, "%s: parent not cleared", name);
        uint8_t *o2 = NULL;
        CHECK(fe_save_build(s, &o2, &out_len, err, sizeof(err)), "build after parent edit: %s", err);
        free(o2);
        printf("  parents ok on %s (was %d/%d): Chrom+Sumia caps Str %d Spd %d (before %d %d)\n", name, f0, m0,
               hb ? b.cap[1] : -1, hb ? b.cap[4] : -1, ha ? a.cap[1] : -1, ha ? a.cap[4] : -1);
        break;
    }
    fe_save_free(s);
    free(s);
    free(file);
}

// Forge create/edit, child hair and inherited skills
static void test_forge_child(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    int f0 = s->forge_count, n0 = s->unit_count;
    uint32_t units_len = s->unit_end - s->unit_off;
    int id = fe_forge_create(s, 15, "Super Falchion", 10, 20, 30, err, sizeof(err));  // 15 = Falchion
    CHECK(id >= FE_ITEM_COUNT, "forge create: %s", err);
    if (id >= FE_ITEM_COUNT) {
        int f = fe_forge_of_item(s, id), mt, hit, crit;
        char nm[40];
        fe_item_name(s, id, nm, sizeof(nm));
        fe_forge_bonus(s, f, &mt, &hit, &crit);
        CHECK(s->forge_count == f0 + 1 && fe_forge_base(s, f) == 15, "forge count/base");
        CHECK(!strcmp(nm, "Super Falchion*") && mt == 10 && hit == 20 && crit == 30, "forge %s %d/%d/%d", nm, mt, hit, crit);
        CHECK(fe_item_max_uses(s, id) == 0, "forged Falchion not unbreakable");
        CHECK(s->unit_count == n0 && s->unit_end - s->unit_off == units_len, "units changed by forge");
        fe_convoy_set_items(s, id, 3);
        CHECK(fe_convoy_count_items(s, id) == 3, "forge convoy count");
        int ai = -1;
        for (int i = 0; i < s->unit_count && ai < 0; i++) if (s->units[i].group == FE_GROUP_ARMY) ai = i;
        if (ai >= 0) fe_inv_set(s, &s->units[ai], 0, id, 0);
        fe_forge_edit(s, f, "Falchion EX", 5, 5, 5);
        fe_item_name(s, id, nm, sizeof(nm));
        CHECK(!strcmp(nm, "Falchion EX*"), "forge rename: %s", nm);
        uint8_t *out = NULL; size_t out_len;
        CHECK(fe_save_build(s, &out, &out_len, err, sizeof(err)), "build after forge: %s", err);
        free(out);
        printf("  forge ok: %s (Falchion, unbreakable, +5/+5/+5) in convoy x3 and inventory, %d forges\n", nm, s->forge_count);
    }
    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        if (u->child_off < 0 || u->group == FE_GROUP_RED) continue;
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        uint32_t h = fe_unit_hair(s, u), d = fe_child_default_hair(s, u);
        fe_unit_set_hair(s, u, 0x123456);
        CHECK(fe_unit_hair(s, u) == 0x123456, "%s: hair not set", name);
        fe_unit_set_hair(s, u, h);
        int n = fe_child_inherit_skills(s, u);
        printf("  child ok on %s: hair #%06X (default #%06X), inherited %d skills\n", name, (unsigned)h, (unsigned)d, n);
        break;
    }
    fe_save_free(s);
    free(s);
    free(file);
}

// Play time, chapter records and world map encounters
static void test_records_map(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    uint32_t pt = fe_playtime(s);
    CHECK(pt == (uint32_t)(s->data[1] | s->data[2] << 8 | s->data[3] << 16 | (uint32_t)s->data[4] << 24),
          "play time differs between header and USER");
    uint8_t *before = malloc(s->size);
    memcpy(before, s->data, s->size);
    fe_set_playtime(s, pt + 3600 * 60);
    CHECK(fe_playtime(s) == pt + 3600 * 60, "play time set");
    int n = fe_record_count(s);
    if (n) {
        fe_record r;
        fe_record_get(s, n - 1, &r);
        fe_record r2 = r;
        r2.turns = 1; r2.frames = 60 * 60; r2.unit[0] = 3; r2.cls[0] = 0; r2.unit[1] = -1; r2.cls[1] = -1;
        fe_record_set(s, n - 1, &r2);
        fe_record r3;
        fe_record_get(s, n - 1, &r3);
        CHECK(r3.turns == 1 && r3.frames == 3600 && r3.unit[0] == 3 && r3.unit[1] == -1 && r3.chapter == r.chapter,
              "record round trip");
    }
    int risen = 0, merch = 0;
    for (int loc = 0; loc < s->map_count; loc++) {
        int team = fe_map_encounter(s, loc) == FE_ENC_TEAM;
        fe_map_set_encounter(s, loc, loc % 2 ? FE_ENC_RISEN : FE_ENC_MERCHANT, 12345u + (uint32_t)loc);
        int e = fe_map_encounter(s, loc);
        CHECK(e == (loc % 2 ? FE_ENC_RISEN : FE_ENC_MERCHANT), "encounter at %d: %d", loc, e);
        risen += e == FE_ENC_RISEN; merch += e == FE_ENC_MERCHANT;
        fe_map_set_encounter(s, loc, FE_ENC_NONE, 0);
        CHECK(fe_map_encounter(s, loc) == (team ? FE_ENC_TEAM : FE_ENC_NONE), "encounter clear at %d", loc);
    }
    uint8_t *out = NULL; size_t out_len;
    CHECK(fe_save_build(s, &out, &out_len, err, sizeof(err)), "build after records/map: %s", err);
    free(out);
    free(before);
    printf("  records ok: play time %lu:%02lu, %d records; encounters set/cleared on %d locations (%d Risen, %d merchants)\n",
           (unsigned long)(pt / 216000), (unsigned long)(pt / 3600 % 60), n, s->map_count, risen, merch);
    fe_save_free(s);
    free(s);
    free(file);
}

// Rename a regular character (adds logbook data), renown claims, barracks events
static void test_rename_barracks(const char *path)
{
    size_t len;
    uint8_t *file = read_file(path, &len);
    fe_save *s = malloc(sizeof(fe_save));
    char err[256];
    if (!file || !fe_save_load(s, file, len, err, sizeof(err))) { free(s); free(file); return; }
    int ci = fe_find_unit(s, 3), src = -1;  // Chrom
    for (int i = 0; i < s->unit_count && src < 0; i++) if (s->units[i].log_off >= 0) src = i;
    if (ci >= 0 && s->units[ci].log_off < 0 && src >= 0) {
        fe_stats a, b;
        bool ha = fe_unit_stats(s, &s->units[ci], &a);
        uint32_t hair = fe_unit_hair(s, &s->units[ci]);
        CHECK(fe_unit_add_logbook(s, ci, src, err, sizeof(err)), "add logbook to Chrom: %s", err);
        const fe_unit *u = &s->units[ci];
        fe_look_set(s, u, 0x1A, 0);
        fe_look_set(s, u, 0x1B, 0);
        fe_unit_set_hair(s, u, hair);
        fe_unit_set_name(s, u, "Exalt Chrom");
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        bool hb = fe_unit_stats(s, u, &b);
        CHECK(!strcmp(name, "Exalt Chrom"), "rename: %s", name);
        CHECK(fe_unit_hair(s, u) == hair, "rename changed hair");
        CHECK(ha == hb && (!ha || memcmp(a.value, b.value, sizeof(a.value)) == 0), "rename changed stats");
        CHECK(fe_unit_char_id(s, u) == 3, "rename changed the character");
        uint8_t *out = NULL; size_t ol;
        CHECK(fe_save_build(s, &out, &ol, err, sizeof(err)), "build after rename: %s", err);
        free(out);
        printf("  rename ok: Chrom -> %s (stats and hair unchanged)\n", name);
    }
    int claimed = fe_renown_claimed(s);
    fe_renown_reset_claims(s);
    CHECK(fe_renown_claimed(s) == 0, "renown claims not reset");
    if (fe_barracks_ok(s)) {
        fe_barracks_event ev = {FE_BEV_TALK, {-1, 3}, 0xFF}, back;  // unit 2 only: moves to unit 1
        fe_barracks_set(s, 2, &ev);
        fe_barracks_get(s, 2, &back);
        CHECK(back.type == FE_BEV_TALK && back.unit[0] == 3 && back.unit[1] == -1, "barracks event shift");
        uint8_t *out = NULL; size_t ol;
        CHECK(fe_save_build(s, &out, &ol, err, sizeof(err)), "build after barracks: %s", err);
        free(out);
    }
    printf("  renown/barracks ok: %d claimed rewards reset, barracks event set\n", claimed);
    fe_save_free(s);
    free(s);
    free(file);
}

static void test_huffman_stress(void)
{
    printf("=== Huffman stress\n");
    int cases = 0;
    for (int mode = 0; mode < 5; mode++) {
        for (int rep = 0; rep < 40; rep++) {
            size_t n = 1 + rng() % 60000;
            uint8_t *in = malloc(n);
            for (size_t i = 0; i < n; i++) {
                switch (mode) {
                case 0: in[i] = (uint8_t)rng(); break;                          // uniform, 256 symbols
                case 1: in[i] = (uint8_t)(rng() % 3); break;                    // tiny alphabet
                case 2: { int b = 0; while ((rng() & 1) && b < 255) b++; in[i] = (uint8_t)b; } break;  // geometric: deep tree
                case 3: in[i] = (rng() % 10) ? 0 : (uint8_t)rng(); break;       // save-like: mostly zero
                case 4: in[i] = (uint8_t)(rng() % (1 + rep * 6)); break;        // growing alphabets
                }
            }
            if (rep == 0 && mode == 1) { n = 1; in[0] = 7; }                     // single byte, one symbol

            uint8_t *c; size_t cl;
            huff_result r = huff_compress(in, n, &c, &cl);
            CHECK(r == HUFF_OK, "compress mode %d rep %d: %s", mode, rep, huff_strerror(r));
            if (r == HUFF_OK) {
                uint8_t *d = malloc(n);
                r = huff_decompress(c, cl, d, n);
                CHECK(r == HUFF_OK && memcmp(d, in, n) == 0, "round trip mode %d rep %d n %u: %s",
                      mode, rep, (unsigned)n, huff_strerror(r));
                free(d);
                free(c);
            }
            free(in);
            cases++;
        }
    }
    printf("  %d cases run\n", cases);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered, so a crash still shows how far it got
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--global=", 9) == 0) {
            test_global(argv[i] + 9);
            snprintf(g_global_path, sizeof(g_global_path), "%s", argv[i] + 9);
            continue;
        }
        if (g_import_count < 16) snprintf(g_import_paths[g_import_count++], 512, "%s", argv[i]);
        test_save(argv[i]);
        test_skills(argv[i]);
        test_class(argv[i]);
        test_supports(argv[i]);
        test_marriage_guard(argv[i]);
        test_items(argv[i]);
        test_avatar(argv[i]);
        test_add_units(argv[i]);
        test_unit_tools(argv[i]);
        test_difficulty_parents(argv[i]);
        test_forge_child(argv[i]);
        test_records_map(argv[i]);
        test_rename_barracks(argv[i]);
    }
    printf("=== Import Avatars\n");
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "--", 2) != 0 && strstr(argv[i], "Chapter0-fem")) test_import(argv[i]);
    test_support_ranks();
    test_huffman_stress();
    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
