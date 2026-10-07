#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/fedata.h"

// In-memory Chapter/Map save: the 0xC0 header followed by the decompressed
// payload, exactly as FEAST's "_dec" files. v1 only patches fixed-size fields
// in place, so block sizes and the INDE index never change.

#define FE_HEADER_US     0xC0
#define FE_MAX_GROUPS    6
#define FE_MAX_UNITS     (FE_MAX_GROUPS * 255)
#define FE_GOLD_MAX      999999u

// Unit groups inside the UNIT block
enum {
    FE_GROUP_BLUE  = 0,  // player units deployed on the map
    FE_GROUP_RED   = 1,  // enemies (map saves)
    FE_GROUP_GREEN = 2,  // allies/others (map saves)
    FE_GROUP_ARMY  = 3,  // main army
    FE_GROUP_DEAD  = 4,  // fallen (Classic mode)
    FE_GROUP_OTHER = 5,
};

typedef struct {
    uint32_t off;        // record start in fe_save.data
    uint32_t size;       // full record size, including trailers
    uint8_t group;
    int32_t log_off;     // logbook trailer (Avatar / logbook units), -1 if none
    int32_t child_off;   // child trailer (parents' data), -1 if none
    uint32_t flags_off;  // 0x2A-byte flags block (temporary buffs live here)
} fe_unit;

typedef struct {
    uint8_t *data;       // header + decompressed payload (owned)
    uint32_t size;
    uint32_t hdr_size;   // 0xC0
    bool is_map;         // Map0/Map1 suspend file rather than ChapterN
    uint32_t user_off, user_end;
    uint32_t unit_off, unit_end;
    uint32_t gold_off;   // absolute offset of the u32 gold value
    uint32_t gmap_off;   // world map block (0 if not understood)
    int map_count;       // world map locations (51 in vanilla)
    uint32_t refi_off;   // forged weapons block
    int forge_count;
    uint32_t tran_off;   // convoy block
    int convoy_count;    // entries in the convoy table (202 regular + 150 forged in vanilla)
    int unit_count;
    fe_unit units[FE_MAX_UNITS];
} fe_save;

// Parses a compressed save file (as stored in the save archive).
// On failure returns false and writes a human-readable reason to err.
bool fe_save_load(fe_save *s, const uint8_t *file, size_t len, char *err, size_t errlen);
// True if the save has more item ids or characters than the base game (a romfs mod such as Thabes').
bool fe_save_is_modded(const fe_save *s);
void fe_save_free(fe_save *s);

// Recompresses the save into a new malloc'd buffer ready to write back.
// Before returning, it decompresses the result again and checks that it
// matches the edited data byte for byte, so a broken encoder can't reach the cart.
bool fe_save_build(const fe_save *s, uint8_t **out, size_t *out_len, char *err, size_t errlen);

uint32_t fe_get_gold(const fe_save *s);
void fe_set_gold(fe_save *s, uint32_t gold);

// Renown: u32 right after gold (USER tail +0x6D). The game caps it at 99,999
// (seen in a real save), so the editor clamps there too.
#define FE_RENOWN_MAX 99999u
uint32_t fe_get_renown(const fe_save *s);
void fe_set_renown(fe_save *s, uint32_t renown);

//--- per-unit fields ---------------------------------------------------------
uint16_t fe_unit_char_id(const fe_save *s, const fe_unit *u);
uint8_t fe_unit_class_id(const fe_save *s, const fe_unit *u);
uint8_t fe_unit_level(const fe_save *s, const fe_unit *u);
uint8_t fe_unit_exp(const fe_save *s, const fe_unit *u);

// Display name in UTF-8: the custom name for the Avatar/logbook units,
// otherwise the character name from the table.
void fe_unit_name(const fe_save *s, const fe_unit *u, char *out, size_t outlen);
const char *fe_class_name(uint8_t class_id);
const char *fe_group_name(uint8_t group);

// Highest level for the unit's class (20, or 30 for special classes).
int fe_unit_level_cap(const fe_save *s, const fe_unit *u);
// Clamps to 1..cap. At the cap, EXP is reset to 0 like the game does.
int fe_unit_set_level(fe_save *s, const fe_unit *u, int level);

//--- stats as shown in game ---------------------------------------------------
typedef struct {
    int value[FE_STAT_COUNT];  // permanent stat (what edits change), capped
    int buff[FE_STAT_COUNT];   // temporary/skill bonus on top (tonics, barracks, rallies, stat skills)
    int base[FE_STAT_COUNT];   // value with zero level-up gains
    int cap[FE_STAT_COUNT];    // class max + personal modifiers (+10 with Limit Breaker)
    bool limit_breaker;
} fe_stats;

// What the game's stat screen shows: value + buff. (Weapon stat bonuses are not included yet.)
static inline int fe_stat_shown(const fe_stats *st, int i) { return st->value[i] + st->buff[i]; }

// Returns false when the character or class ID isn't in the tables
// (story enemies, modded saves); such units are shown but not stat-editable.
bool fe_unit_stats(const fe_save *s, const fe_unit *u, fe_stats *out);

//--- skills -----------------------------------------------------------------
// Equipped skills: 5 slots at unit +0x33 (2 bytes each, id in the low byte).
// Learned skills: 13-byte bit list right after the flags block; skill n = bit n%8 of byte n/8.
// The game's skill menu lists the learned skills, so anything learned can be swapped in game.
#define FE_EQUIP_SLOTS  5
#define FE_SKILL_FIRST  1    // 0 = "No Active Skill"
#define FE_SKILL_LAST   102  // 103 = "Unused Skill Slot"

void fe_unit_equipped(const fe_save *s, const fe_unit *u, int out[FE_EQUIP_SLOTS]);  // 0 = empty
bool fe_unit_is_equipped(const fe_save *s, const fe_unit *u, int skill);
bool fe_unit_is_learned(const fe_save *s, const fe_unit *u, int skill);
int fe_unit_learned_count(const fe_save *s, const fe_unit *u);

// Unlearning an equipped skill also unequips it.
void fe_unit_set_learned(fe_save *s, const fe_unit *u, int skill, bool learned);
// Puts the skill in the first free slot and marks it learned.
// Returns the slot, or -1 if it is already equipped, all 5 slots are full, or the id is invalid.
int fe_unit_equip(fe_save *s, const fe_unit *u, int skill);
// Removes the skill from its slot (it stays learned). Filled slots are kept first,
// because an empty slot before a filled one glitches the in-game skill menu.
void fe_unit_unequip(fe_save *s, const fe_unit *u, int skill);
// Marks every skill FE_SKILL_FIRST..FE_SKILL_LAST learned. Returns how many were newly learned.
int fe_unit_learn_all(fe_save *s, const fe_unit *u);

//--- supports ---------------------------------------------------------------
// Each unit stores one byte of support points per partner (fe_supports[char]),
// but only as many bytes as the game has needed so far; missing ones count as 0.
// Raising a support past the stored length grows the unit record, so everything
// after it moves and the block index is rewritten. That is done on a copy and the
// whole layout is re-parsed before the change is accepted.

// Points with this partner character, or -1 if the partner is not in the unit's list.
int fe_support_get(const fe_save *s, const fe_unit *u, int partner_char);
// Sets the points on both sides (the partner's record too, if that unit is in the save).
// May grow the save. On failure nothing is changed.
bool fe_support_set(fe_save *s, int unit_index, int partner_char, int value, char *err, size_t errlen);
// Rank names. The game shows a conversation as ready ("!") when the points are exactly
// at a "ready" value; watching it adds 1. Ready values per support type (C, B, A, S):
//   0 non-romantic 3, 9, 17 (no S) | 1 slow 4, 9, 15, 21 | 2 medium 3, 8, 13, 19 | 3 fast 2, 7, 12, 17
// Confirmed on hardware for C (Frederick-Bond 3 = "!", Chrom-Frederick 4 = C reached).
// Writes e.g. "-", "C", "B ready", "S" into out.
void fe_support_rank_name(int type, int value, char *out, size_t outlen);
// Every stage a pair can be set to, in order: 0, C ready, C, B ready, B, A ready, A
// (, S ready, S for romance). "Reached" ranks are ready + 1, i.e. the value the game
// stores after the conversation is watched, so setting one skips the conversation.
// Next/previous stage after/before `value` (-1 if none above; 0 if none below).
int fe_support_next_step(int type, int value);
int fe_support_prev_step(int type, int value);

// Next "ready" value above `value` (-1 if none), and the previous one below it (0 if none).
int fe_support_next_ready(int type, int value);
int fe_support_prev_ready(int type, int value);
// Points of the "A" rank (A ready + 1) for a support type.
int fe_support_a_rank(int type);
// Sets only this unit's side of a pair (may grow the save).
bool fe_support_set_own(fe_save *s, int unit_index, int partner_char, int value, char *err, size_t errlen);

// Marriage. A pair at S rank (points above "S ready") is married. Watching another S
// conversation while either unit is married undoes that marriage in game (hardware test
// 2026-10-02), so "S ready" pairs involving a married unit are a trap.
// Character id this unit is married to (S rank), or -1. `except` is ignored as a spouse.
int fe_support_spouse(const fe_save *s, int unit_index, int except_char);
// True if raising this pair to "S ready" would undo an existing marriage.
bool fe_support_s_would_unmarry(const fe_save *s, int unit_index, int partner_char);
// Lowers every "S ready" pair where either unit is already married to someone else down
// to A rank (A ready + 1, so no conversation is pending), on both sides.
// Writes one line per change into report (may be NULL). Returns the number of pairs changed.
int fe_support_fix_pending_s(fe_save *s, char *report, size_t report_len);

// Player unit (deployed/army/fallen) with this character id, or -1.
int fe_find_unit(const fe_save *s, int char_id);

//--- inventory --------------------------------------------------------------
// 5 slots at unit +0x1A, 5 bytes each: 0x04, u16 item id, uses, flags (0x10 = equipped,
// 0x20 = dropped by an enemy). Item ids >= FE_ITEM_COUNT are forged weapons
// (forge index = id - FE_ITEM_COUNT). Items are kept packed at the top.
#define FE_INV_SLOTS 5
#define FE_ITEM_EQUIPPED 0x10
typedef struct {
    int id;
    int uses;
    int flags;
} fe_inv_item;

void fe_inv_get(const fe_save *s, const fe_unit *u, int slot, fe_inv_item *out);
// Puts an item in a slot (id 0 removes it). Uses are clamped to 0..255. Slots stay packed.
void fe_inv_set(fe_save *s, const fe_unit *u, int slot, int id, int uses);
// Marks this slot equipped and moves it to the top, like the game does. Others lose the flag.
void fe_inv_equip(fe_save *s, const fe_unit *u, int slot);
// Name of any item id, including forged weapons ("Excalibur*" for a forge).
void fe_item_name(const fe_save *s, int id, char *out, size_t outlen);
// Max uses for an item id (forges use their base weapon's); 0 = unbreakable.
int fe_item_max_uses(const fe_save *s, int id);

//--- forged weapons -----------------------------------------------------------
// Forge record (US, 0x2C bytes): +0 index, +2 UTF-16 name (18 chars), +0x26 u16 base weapon id,
// +0x28 might, +0x29 hit, +0x2A crit bonuses.
void fe_forge_name(const fe_save *s, int forge, char *out, size_t outlen);
int fe_forge_base(const fe_save *s, int forge);
// Forge editing. Bonuses are added to the base weapon's Might/Hit/Crit; uses always come from
// the base weapon (a forged Falchion stays unbreakable). Up to 150 forges, 17-char names.
#define FE_FORGE_MAX       150
#define FE_FORGE_NAME_MAX  17
int fe_forge_of_item(const fe_save *s, int item_id);  // forge record index for a forged item id, or -1
void fe_forge_bonus(const fe_save *s, int forge, int *might, int *hit, int *crit);
void fe_forge_edit(fe_save *s, int forge, const char *name, int might, int hit, int crit);
// Creates a forge of `base` and returns its item id (FE_ITEM_COUNT + slot), or -1.
int fe_forge_create(fe_save *s, int base, const char *name, int might, int hit, int crit, char *err, size_t errlen);

//--- convoy -------------------------------------------------------------------
// One u16 per item id: total uses held (items x max uses; for unbreakable items the count).
int fe_convoy_get(const fe_save *s, int id);
void fe_convoy_set(fe_save *s, int id, int total_uses);
// The same as a number of items (rounded up) and setting a number of items.
int fe_convoy_count_items(const fe_save *s, int id);
void fe_convoy_set_items(fe_save *s, int id, int count);

//--- Avatar / logbook appearance ---------------------------------------------
// Logbook trailer (units with log_off >= 0: the Avatar, logbook units, DLC heroes):
// +0x00 UTF-16 name (12 chars + 0), then +0x1A asset, +0x1B flaw, +0x1C gender (0 m / 1 f),
// +0x1D build 0-2, +0x1E face (0-4, or a DLC hero face id), +0x1F hair 0-4,
// +0x20 hair color R,G,B,A, +0x24 voice 0-4, +0x25 birthday day, +0x26 month.
enum {
    FE_LOOK_GENDER = 0x1C, FE_LOOK_BUILD = 0x1D, FE_LOOK_FACE = 0x1E, FE_LOOK_HAIR = 0x1F,
    FE_LOOK_COLOR = 0x20, FE_LOOK_VOICE = 0x24, FE_LOOK_DAY = 0x25, FE_LOOK_MONTH = 0x26,
};
#define FE_NAME_MAX 12
// Face ids the DLC heroes use (Avatar model with a special head). 11 = Marth.
#define FE_HERO_FACE_COUNT 16
extern const uint8_t fe_hero_faces[FE_HERO_FACE_COUNT];

// Logbook identity: 13 bytes at logbook +0x27 (mainBlock 0x0D). The Avatar has a random
// personal id; DLC/SpotPass heroes have their hero id in the first byte (Pr. Marth = 0xC9)
// and bit 1 of logbook +0x37 (mainBlock 0x1D, "Einherjar") set. The portrait follows the
// face byte, but the 3D head seems to follow this identity (hardware test 2026-10-02).
#define FE_LOG_ID_SIZE 13
void fe_log_id_get(const fe_save *s, const fe_unit *u, uint8_t out[FE_LOG_ID_SIZE]);
void fe_log_id_set(fe_save *s, const fe_unit *u, const uint8_t id[FE_LOG_ID_SIZE]);
bool fe_log_hero_flag(const fe_save *s, const fe_unit *u);
void fe_log_set_hero_flag(fe_save *s, const fe_unit *u, bool on);
// The hero preset whose identity this unit carries (id + flag), or NULL.
const fe_look_preset *fe_log_hero_identity(const fe_save *s, const fe_unit *u);
// Gives the unit a hero's identity: hero id in byte 0, rest 0, hero flag set.
void fe_log_set_hero_identity(fe_save *s, const fe_unit *u, const fe_look_preset *p);

// Hero name for a hero face id ("Marth"), or NULL for a normal face.
const char *fe_hero_face_name(int face);
// Applies a whole look (gender, build, face, hairstyle, hair color, voice).
void fe_look_apply_preset(fe_save *s, const fe_unit *u, const fe_look_preset *p);

int fe_look_get(const fe_save *s, const fe_unit *u, int field);           // -1 if no logbook data
void fe_look_set(fe_save *s, const fe_unit *u, int field, int value);    // byte fields
uint32_t fe_look_color(const fe_save *s, const fe_unit *u);              // 0xRRGGBB
void fe_look_set_color(fe_save *s, const fe_unit *u, uint32_t rgb);
// Renames a logbook unit (UTF-8 in, BMP only, max FE_NAME_MAX characters).
// If it is the slot's main Avatar, the name in the slot header (shown on the game's
// file-select screen) is updated too, but only if it matched the old name.
// Returns the number of characters written, or -1 if the unit has no logbook data.
int fe_unit_set_name(fe_save *s, const fe_unit *u, const char *utf8);

//--- adding units ------------------------------------------------------------
// Adds a fresh level-1 recruit of `char_id` to the army (group 3), built from the record
// of `template_index` (an existing unit; its weapon ranks, flags and AI block are reused):
// supports cleared, trailers removed, level 1, no gains, empty inventory, the character's
// starting class and personal skills, full HP. Children (fe_recruit[].parent >= 0) get a
// child block: the fixed parent plus that parent's S-rank spouse (if any) as the other
// parent, with their asset/flaw. The save grows; everything after the new unit moves and
// the block index is rewritten, then the layout is re-parsed (like support growth).
// Returns the new unit's index, or -1 with err set.
int fe_unit_add(fe_save *s, int template_index, int char_id, char *err, size_t errlen);
// Gives a unit without trailers an Avatar logbook block (name, look, logbook id), copied
// from `src_index`'s logbook and then set to: name "Robin", gender from the character,
// face/hairstyle 0, build 1, voice 0, default white hair (both copies), a new random
// logbook id, hero flag off. Without it the game shows an Avatar as "Unknown" with a
// silhouette (hardware test 2026-10-02). The save grows by 0x188 bytes.
bool fe_unit_add_logbook(fe_save *s, int unit_index, int src_index, char *err, size_t errlen);
// Characters the Add-a-unit list should not offer: Logbook Unit (2), Dummy/Maiden (53),
// Risen (54, 55), Merchant (56). They came out as male-Robin lookalikes in testing.
bool fe_char_addable(int char_id);

//--- weapon ranks, Boots, battle records ----------------------------------------
// Weapon exp: 6 bytes after the equipped skills, half the game's points. Rank thresholds
// E 0, D 15, C 35, B 60, A 90 (A is Awakening's top rank; matches FireEditor and saves).
#define FE_WEAPON_COUNT 6
#define FE_WEXP_MAX     90
extern const char *const fe_weapon_names[FE_WEAPON_COUNT];
int fe_unit_wexp(const fe_save *s, const fe_unit *u, int weapon);
void fe_unit_set_wexp(fe_save *s, const fe_unit *u, int weapon, int value);  // clamped 0..90
const char *fe_wexp_rank(int value);  // "E".."A"
int fe_wexp_next_rank(int value);     // next threshold above value (90 at the top)
int fe_wexp_prev_rank(int value);     // previous threshold below value (0 at the bottom)
// Extra movement from Boots (unit +0x15).
int fe_unit_boots(const fe_save *s, const fe_unit *u);
void fe_unit_set_boots(fe_save *s, const fe_unit *u, int value);
// Battles / victories (u16 each, end block +0x31/+0x33).
int fe_unit_record(const fe_save *s, const fe_unit *u, bool victories);
void fe_unit_set_record(fe_save *s, const fe_unit *u, bool victories, int value);
// Sets current HP to max. Returns true if it changed.
bool fe_unit_heal(fe_save *s, const fe_unit *u);

//--- moving / removing units -----------------------------------------------------
// Both rebuild the UNIT block (verified by re-parsing; nothing changes on failure).
// Unit indices after unit_index shift, so re-read pointers afterwards.
// Revive: a fallen (Classic) unit loses its fallen flags, gets full HP and joins the
// end of the army.
bool fe_unit_revive(fe_save *s, int unit_index, char *err, size_t errlen);

//--- world map and story progress ------------------------------------------------
// GMAP: 0x3E-byte header (location count at +0x3D), then one 0x1D-byte entry per location;
// entry +1 = state. Location i is the chapter whose map id is i (fe_map_name).
// USER +0xF = current story chapter id (FireEditor chapters.xml ids: 2 Prologue,
// 3 Chapter 1 ... 28 Endgame, 29+ paralogues).
enum { FE_MAP_LOCKED = 0, FE_MAP_BEATEN = 1, FE_MAP_OPEN = 2 };
int fe_map_state(const fe_save *s, int loc);
void fe_map_set_state(fe_save *s, int loc, int state);
const char *fe_map_name(int loc);   // "Chapter 3", "Paralogue 5", "Map #52"...
const char *fe_map_place(int loc);  // "The Longfort"... ("" if unknown)
// Encounters: each location has two 0x0D slots at entry +3 / +0x10: [type a, type b] (1,1 Risen,
// 3,2 merchant, 2,2 StreetPass/SpotPass team), u16 map class, u16 pool, days left, team id,
// u32 seed, slot flag. Teams are never touched by the setter.
enum { FE_ENC_NONE = 0, FE_ENC_RISEN, FE_ENC_MERCHANT, FE_ENC_TEAM };
int fe_map_encounter(const fe_save *s, int loc);  // Risen/merchant first, then team, else none
void fe_map_set_encounter(fe_save *s, int loc, int type, uint32_t random);
// Play time in frames (60 per second): u32 at USER +5 and file header +1 (both written).
uint32_t fe_playtime(const fe_save *s);
void fe_set_playtime(fe_save *s, uint32_t frames);
// Chapter records (credits): USER +0x15 count, then 0x10 bytes each from +0x16:
// [1][chapter id][u16 turns][u32 frames][u16 unit, u16 class][u16 unit, u16 class] (0xFFFF none).
typedef struct {
    int chapter, turns;
    uint32_t frames;
    int unit[2], cls[2];  // -1 = none
} fe_record;
int fe_record_count(const fe_save *s);
void fe_record_get(const fe_save *s, int k, fe_record *out);
void fe_record_set(fe_save *s, int k, const fe_record *r);
// Renown rewards: 5 bytes of "claimed" bits right after renown (USER tail +0x7B). Clearing
// them lets the Renown menu give every reward again.
int fe_renown_claimed(const fe_save *s);  // how many rewards are marked claimed
void fe_renown_reset_claims(fe_save *s);

// Barracks events (EVST block): 5 slots of 8 bytes after a 0x50-byte header:
// [0] ?, [1] unit count, [2] type, u16 unit 1, u16 unit 2 (character ids, 0xFFFF none), [7] icon.
#define FE_BARRACKS_SLOTS 5
enum { FE_BEV_NONE, FE_BEV_STAT, FE_BEV_EXP, FE_BEV_WEXP, FE_BEV_ITEM, FE_BEV_TALK, FE_BEV_BIRTHDAY, FE_BEV_TYPES };
extern const char *const fe_barracks_names[FE_BEV_TYPES];
typedef struct { int type, unit[2], icon; } fe_barracks_event;
bool fe_barracks_ok(const fe_save *s);
void fe_barracks_get(const fe_save *s, int slot, fe_barracks_event *out);
void fe_barracks_set(fe_save *s, int slot, const fe_barracks_event *ev);
int fe_story_chapter(const fe_save *s);  // chapter id
void fe_story_set_chapter(fe_save *s, int chapter_id);
const char *fe_chapter_name(int chapter_id);  // by chapter id
bool fe_unit_remove(fe_save *s, int unit_index, char *err, size_t errlen);
//--- difficulty and mode ---------------------------------------------------------
// Stored twice: file header +0x08 (mode flags, 0x04 = Casual), +0x09 (bit 0 = Lunatic+),
// +0x0D (difficulty 0 Normal, 1 Hard, 2 Lunatic), and the same bytes in the USER tail
// (gold - 6, gold - 5, gold - 1). Setters write both copies.
int fe_difficulty(const fe_save *s);
void fe_set_difficulty(fe_save *s, int difficulty);
bool fe_is_casual(const fe_save *s);
void fe_set_casual(fe_save *s, bool casual);
bool fe_is_lunatic_plus(const fe_save *s);
void fe_set_lunatic_plus(fe_save *s, bool on);

//--- parents of a child unit -------------------------------------------------------
// Child data: father at +0x01, mother at +0x12 (0x11 bytes each): a support byte, then 3
// entries of 5 bytes from +2: the parent, then the parent's own father and mother
// (u16 char id or 0xFFFF, asset, flaw, extra).
int fe_child_parent(const fe_save *s, const fe_unit *u, int side);  // side 0 father, 1 mother; -1 none
// Sets a parent by character id (-1 = none). Asset/flaw come from that character's Avatar
// look data when it is in the save, grandparents from that unit's parents when it is a child.
void fe_child_set_parent(fe_save *s, const fe_unit *u, int side, int char_id);
// Hair color (0xRRGGBB) of any unit: the unit copy (end block +0x39); setting it also updates
// the logbook copy of Avatar/logbook units.
uint32_t fe_unit_hair(const fe_save *s, const fe_unit *u);
void fe_unit_set_hair(fe_save *s, const fe_unit *u, uint32_t rgb);
// The hair color the game gives this child: the non-fixed parent's hair for children whose
// hair varies (Inigo, Morgan...), else the character's own color.
uint32_t fe_child_default_hair(const fe_save *s, const fe_unit *u);
// Learns every skill that the child's parents (in this save) have learned. Returns how many
// were new.
int fe_child_inherit_skills(fe_save *s, const fe_unit *u);

// Inserts a complete unit record (as stored in a save) at the end of the army.
// Returns the new unit's index, or -1 (nothing changed).
int fe_unit_insert(fe_save *s, const uint8_t *rec, uint32_t len, char *err, size_t errlen);
// Base weapon of a forged item id (or the id itself for a regular item; 0 if unknown).
int fe_item_base(const fe_save *s, int id);
// Deep copy (for previews). Returns false when out of memory.
bool fe_save_clone(const fe_save *src, fe_save *dst);
// Ids past the table crash the game on load (hardware tests 2026-10-02: 57-~156 and
// 332-352): the save's character id indexes the playable-character table only.
#define FE_CHAR_EXPLORE_MAX (FE_CHAR_COUNT - 1)

// For a child character: the parents fe_unit_add would use (-1 = none).
void fe_unit_add_parents(const fe_save *s, int char_id, int *father, int *mother);

//--- class ------------------------------------------------------------------
// Changes the class byte. Any class 0..FE_CLASS_COUNT-1 is allowed (no gender or
// enemy-class restrictions, by design). Stored level-up gains are kept, so the
// in-game stats shift by the class base difference, exactly like an in-game reclass.
// The level is clamped to the new class's cap (EXP reset at the cap), and a unit at
// full HP stays at full HP. Returns false for an invalid class id.
bool fe_unit_set_class(fe_save *s, const fe_unit *u, int class_id);
// The unit's gender as implied by its current class.
bool fe_unit_is_female(const fe_save *s, const fe_unit *u);

// Sets one displayed stat (clamped to base..cap) by rewriting the stored gain.
// Changing HP also keeps current HP consistent. Returns the new displayed value, or -1.
int fe_unit_set_stat(fe_save *s, const fe_unit *u, int stat, int value);
