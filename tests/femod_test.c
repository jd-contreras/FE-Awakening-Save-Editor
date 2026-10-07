// Host test for romfs mod loading (femod.c).
//   femod_test <vanilla romfs dir> <mod romfs dir> [modded save file]
// The vanilla romfs must reproduce the built-in tables; the mod romfs (Thabes' mod) is checked
// against known values (Thabes' mod); a modded save is opened with the mod active.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/femod.h"
#include "core/fesave.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void check_vanilla(const char *dir)
{
    printf("=== vanilla romfs %s\n", dir);
    femod m;
    char err[200];
    if (!femod_load(&m, dir, err, sizeof(err))) { CHECK(0, "load: %s", err); return; }
    CHECK(m.active && m.has_persons && m.has_gamedata && m.has_names, "all three files loaded");
    CHECK(m.char_count == FE_CHAR_COUNT, "char count %d", m.char_count);
    CHECK(m.class_count == FE_CLASS_COUNT, "class count %d", m.class_count);
    CHECK(m.item_count == FE_ITEM_COUNT, "item count %d", m.item_count);
    CHECK(!strcmp(m.label, "Modded"), "label %s", m.label);
    for (int i = 0; i < m.char_count && i < FE_CHAR_COUNT; i++) {
        CHECK(m.char_vanilla[i] == i, "char %d maps to vanilla %d", i, m.char_vanilla[i]);
        CHECK(!memcmp(m.chars[i].additions, fe_chars[i].additions, 8), "char %d additions", i);
        CHECK(!memcmp(m.chars[i].modifiers, fe_chars[i].modifiers, 8), "char %d modifiers", i);
        const fe_support_list *a = &m.supports[i], *b = &fe_supports[i];
        CHECK(a->count == b->count, "char %d support count %d vs %d", i, a->count, b->count);
        for (int k = 0; k < a->count && k < b->count; k++)
            CHECK(a->partners[k].partner == b->partners[k].partner && a->partners[k].type == b->partners[k].type,
                  "char %d slot %d", i, k);
    }
    // Known differences between FireEditor's classes.xml and the game's GameData (the game's
    // values are what the game uses): Thief (M/F) caps Skl 29 / Def 21, Taguel (F) bases.
    for (int i = 0; i < m.class_count && i < FE_CLASS_COUNT; i++) {
        bool known_caps = i == 34 || i == 35, known_bases = i == 70;
        if (known_caps || known_bases) printf("  note: class %d (%s) differs from the editor table (known)\n", i, fe_classes[i].name);
        CHECK(known_bases || !memcmp(m.classes[i].base, fe_classes[i].base, 8), "class %d bases", i);
        CHECK(known_caps || !memcmp(m.classes[i].max, fe_classes[i].max, 8), "class %d caps", i);
        CHECK(m.classes[i].promoted == fe_classes[i].promoted, "class %d promoted", i);
    }
    int name_diff = 0;
    for (int i = 1; i < m.item_count; i++) {
        CHECK(m.items[i].type == fe_items[i].type, "item %d type %d vs %d", i, m.items[i].type, fe_items[i].type);
        CHECK(m.items[i].uses == fe_items[i].uses, "item %d uses", i);
        if (strcmp(m.items[i].name, fe_items[i].name)) {
            if (name_diff++ < 5) printf("  note: item %d game name \"%s\", editor \"%s\"\n", i, m.items[i].name, fe_items[i].name);
        }
    }
    printf("  item names differing from the editor's: %d\n", name_diff);
    CHECK(!femod_differs_from_vanilla(&m), "vanilla romfs counts as vanilla");
    femod_free(&m);
}

static void check_mod(const char *dir, const char *save_path)
{
    printf("=== mod romfs %s\n", dir);
    femod m;
    char err[200];
    if (!femod_load(&m, dir, err, sizeof(err))) { CHECK(0, "load: %s", err); return; }
    printf("  label %s: %d characters, %d classes, %d regular items\n", m.label, m.char_count, m.class_count, m.item_count);
    CHECK(!strcmp(m.label, "Modded"), "label");
    CHECK(femod_differs_from_vanilla(&m), "the mod counts as modded");
    CHECK(m.char_count == 67 && m.class_count == 118 && m.item_count == 306, "counts");
    CHECK(m.char_vanilla[62] == 52, "Pr. Marth is 62 (vanilla 52), got %d", m.char_vanilla[62]);
    CHECK(m.char_vanilla[52] == -1 && !strcmp(m.chars[52].name, "Mustafa"), "52 is Mustafa: %s", m.chars[52].name);
    CHECK(!strcmp(m.items[202].name, "Blizzard"), "item 202 %s", m.items[202].name);
    CHECK(m.supports[3].count == 52, "Chrom has 52 supports (%d)", m.supports[3].count);
    printf("  new characters:");
    for (int i = 0; i < m.char_count; i++) if (m.char_vanilla[i] < 0) printf(" %d=%s", i, m.chars[i].name);
    printf("\n  new classes:");
    for (int i = FE_CLASS_COUNT; i < m.class_count; i++) printf(" %d=%s;", i, m.classes[i].name);
    printf("\n  some new items:");
    for (int i = 202; i < m.item_count && i < 222; i++) printf(" %d=%s;", i, m.items[i].name);
    printf("\n");

    if (save_path) {
        femod_set_active(&m);
        FILE *f = fopen(save_path, "rb");
        if (!f) { CHECK(0, "missing %s", save_path); femod_free(&m); return; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n);
        fread(buf, 1, n, f); fclose(f);
        fe_save *s = calloc(1, sizeof(fe_save));
        if (!fe_save_load(s, buf, (size_t)n, err, sizeof(err))) { CHECK(0, "save: %s", err); }
        else {
            printf("  save %s: convoy %d, %d units\n", save_path, s->convoy_count, s->unit_count);
            for (int i = 0; i < s->unit_count; i++) {
                char nm[48], cls[48];
                fe_unit_name(s, &s->units[i], nm, sizeof(nm));
                snprintf(cls, sizeof(cls), "%s", fe_class_name(fe_unit_class_id(s, &s->units[i])));
                fe_stats st;
                bool ok = fe_unit_stats(s, &s->units[i], &st);
                printf("    %-14s char %2d  %-22s HP %d%s\n", nm, fe_unit_char_id(s, &s->units[i]), cls,
                       ok ? st.value[0] : -1, ok ? "" : " (no stats)");
                for (int k = 0; k < 5; k++) {
                    fe_inv_item it;
                    fe_inv_get(s, &s->units[i], k, &it);
                    if (it.id) {
                        char in[48];
                        fe_item_name(s, it.id, in, sizeof(in));
                        printf("        item %d: %s (%d)\n", it.id, in, it.uses);
                    }
                }
            }
        }
        femod_set_active(NULL);
    }
    femod_free(&m);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: femod_test <vanilla romfs> <mod romfs> [save]\n"); return 2; }
    check_vanilla(argv[1]);
    check_mod(argv[2], argc > 3 ? argv[3] : NULL);
    printf(failures ? "\n%d FAILURES\n" : "\nALL PASSED\n", failures);
    return failures != 0;
}
