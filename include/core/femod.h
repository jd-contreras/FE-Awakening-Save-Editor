#pragma once

// Romfs mods (Luma3DS LayeredFS, sdmc:/luma/titles/<TID>/romfs/): reads the mod's character,
// class and item data so names, stats, supports and forge ids match the modded game.
// Files: data/person/static.bin.lz, data/GameData.bin.lz, m/E/GameData.bin.lz (names).
// Each file is optional; anything missing keeps the vanilla tables.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fedata.h"

typedef struct {
    bool active;          // at least one data file was loaded
    bool has_persons, has_gamedata, has_names;
    char label[40];       // shown in the app: "Modded"

    int char_count;
    fe_char_info *chars;
    fe_char_recruit *recruit;
    fe_support_list *supports;
    int16_t *char_vanilla;  // vanilla character id, -1 for characters the mod adds

    int item_count;         // regular items; forged weapons use ids from here on
    fe_item_info *items;
    int class_count;
    fe_class_info *classes;

    char *strings;          // names (UTF-8), owned
    size_t strings_len, strings_cap;
} femod;

// Loads whatever mod files exist under romfs_dir (no trailing slash). Returns false only on a
// read or format error; a folder with no data files gives true with m->active = false.
bool femod_load(femod *m, const char *romfs_dir, char *err, size_t errlen);
void femod_free(femod *m);

// True if the loaded data differs from the vanilla game (counts, character stats or supports,
// class bases/caps, item types/uses). Names are ignored. Used to tell a mod built into the
// installed game from the plain game.
bool femod_differs_from_vanilla(const femod *m);

// The data set the fe_char()/fe_item()/fe_class() accessors use (NULL = vanilla).
void femod_set_active(const femod *m);
const femod *femod_active(void);

// Nintendo LZ11 / LZ13 (0x13 + size, then LZ11) decompression. *out is malloc'd.
bool fe_lz_decompress(const uint8_t *in, size_t n, uint8_t **out, size_t *outn);
