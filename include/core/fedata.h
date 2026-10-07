#pragma once

#include <stdint.h>

// Character and class stat tables (generated into source/core/fedata_tables.c).
// Stat order everywhere: HP, Str, Mag, Skl, Spd, Lck, Def, Res.

#define FE_STAT_COUNT  8
#define FE_CHAR_COUNT  57
#define FE_CLASS_COUNT 83
#define FE_SKILL_COUNT 104  // ids 0..103; 0 = none, 103 = unused slot

// Upper bounds for romfs mods (vanilla counts are the *_COUNT values)
#define FE_MAX_CHARS   250
#define FE_MAX_CLASSES 256
#define FE_MAX_ITEMS   1024

typedef struct {
    const char *name;
    int8_t additions[FE_STAT_COUNT];  // personal base added on top of the class base
    int8_t modifiers[FE_STAT_COUNT];  // personal cap modifiers
} fe_char_info;

typedef struct {
    const char *name;
    int8_t base[FE_STAT_COUNT];
    uint8_t max[FE_STAT_COUNT];
    uint8_t female;      // female-only class (in vanilla, no male model)
    uint8_t promoted;
    uint8_t enemy_only;  // Soldier, Reverant, Entombed, Grima, Mirage, Dummy
} fe_class_info;

typedef struct {
    const char *name;
    uint8_t dlc;  // comes from a DLC item (All Stats +2, Paragon, Iote's Shield, Limit Breaker)
} fe_skill_info;

// Support partners. Slot k of a unit's support block belongs to partners[k].
// type: 0 non-romantic, 1 slow, 2 medium, 3 fast (romantic speeds), as in FireEditor;
// 4 = listed but no rank can be reached (romfs mod placeholders).
#define FE_MAX_SUPPORTS 52       // the game has 52 slots; vanilla uses up to 47
#define FE_SUPPORT_EMPTY 0xFF    // partner of an unused slot (romfs mods can leave gaps)
typedef struct {
    uint8_t partner;  // character id
    uint8_t type;
} fe_support_def;

typedef struct {
    uint8_t count;
    fe_support_def partners[FE_MAX_SUPPORTS];
} fe_support_list;

// Items (vanilla ids 0..201; 0 = none). type: 0 sword, 1 lance, 2 axe, 3 bow, 4 tome,
// 5 staff, 6 dragonstone, 7 beaststone, 8 claws, 9 breath, 10 item.
// uses = max uses of one item; 0 = unbreakable.
#define FE_ITEM_COUNT 202
typedef struct {
    const char *name;
    uint8_t type;
    uint8_t uses;
} fe_item_info;
extern const fe_item_info fe_items[FE_ITEM_COUNT];
extern const char *const fe_item_type_names[11];

// Avatar appearance presets from the SpotPass/DLC hero units (tools/data/einherjar.xml).
// hero_head = 1 for the 16 DLC heroes whose face id is a unique head (Marth, Roy, ...).
#define FE_LOOK_PRESET_COUNT 138
typedef struct {
    const char *name;
    uint8_t gender, build, face, hair, voice;
    uint32_t color;  // 0xRRGGBB
    uint8_t hero_head;
    uint8_t log_id;  // the hero's logbook id (Pr. Marth = 201), stored at logbook +0x27
} fe_look_preset;
extern const fe_look_preset fe_look_presets[FE_LOOK_PRESET_COUNT];
// The same heroes as recruitable units (einherjar.xml): class, level, character id (2 =
// logbook unit, 52 = Marth), asset/flaw, items, stat gains (8-bit, 255 = -1), weapon exp, skills.
typedef struct {
    uint8_t cls, level, char_id, asset, flaw;
    uint8_t items[5];
    uint8_t gains[8];
    uint8_t wexp[6];
    uint8_t skills[5];
} fe_hero_unit;
extern const fe_hero_unit fe_hero_units[FE_LOOK_PRESET_COUNT];

extern const fe_char_info fe_chars[FE_CHAR_COUNT];

// What a fresh recruit of each character looks like (units.xml).
typedef struct {
    uint8_t female;
    int16_t parent;      // fixed parent for children (Morgan (M) -> Avatar (F) = 1), -1 otherwise
    uint8_t start_class;
    uint8_t skills[5];   // personal skills (0 = none)
    uint32_t hair;       // hair color 0xRRGGBB (children: the default when there is no other parent)
    uint8_t custom_hair; // child whose hair comes from the non-fixed parent
} fe_char_recruit;
extern const fe_char_recruit fe_recruit[FE_CHAR_COUNT];
extern const fe_support_list fe_supports[FE_CHAR_COUNT];
extern const fe_skill_info fe_skills[FE_SKILL_COUNT];
extern const fe_class_info fe_classes[FE_CLASS_COUNT];

extern const char *const fe_stat_names[FE_STAT_COUNT];

// ---- Active game data: the tables above, or a romfs mod read from the SD card (femod.h) ----
// Use these instead of the arrays; ids out of range give NULL / -1.
int fe_char_count(void);
const fe_char_info *fe_char(int id);
const fe_char_recruit *fe_char_recruit_of(int id);
const fe_support_list *fe_char_supports(int id);
int fe_char_vanilla_id(int id);         // -1 = a character the mod adds
int fe_char_by_vanilla_id(int vanilla_id);
int fe_item_count(void);                // regular items; forged weapons use ids from here on
const fe_item_info *fe_item(int id);
int fe_class_count(void);
const fe_class_info *fe_class(int id);
