#include "core/fesave.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/crc32.h"
#include "core/huffman.h"

// Block magics as they appear in the file (byte-reversed ASCII)
static const uint8_t MAGIC_COMP[4] = {'P', 'M', 'O', 'C'};
static const uint8_t MAGIC_INDE[4] = {'E', 'D', 'N', 'I'};
static const uint8_t MAGIC_USER[4] = {'R', 'E', 'S', 'U'};
static const uint8_t MAGIC_GMAP[4] = {'P', 'A', 'M', 'G'};
static const uint8_t MAGIC_UNIT[4] = {'T', 'I', 'N', 'U'};
static const uint8_t MAGIC_REFI[4] = {'I', 'F', 'E', 'R'};
static const uint8_t MAGIC_TRAN[4] = {'N', 'A', 'R', 'T'};

#define COMP_TYPE_HUFFMAN8  2
#define COMP_HDR_SIZE       0x10
#define MAX_DECOMPRESSED    (4u * 1024 * 1024)

// Unit record layout (see docs/SAVE_FORMAT.md)
#define UNIT_SUPPORT_LEN_OFF 0x43
#define UNIT_FIXED_SIZE      0xBB
#define UNIT_LOG_SIZE_US     0x188
#define UNIT_CHILD_SIZE      0x25
#define UNIT_CHAR_ID         0x01
#define UNIT_CLASS           0x03
#define UNIT_GAINS           0x0A
#define UNIT_LEVEL           0x12
#define UNIT_EXP             0x13
#define UNIT_CUR_HP          0x14
#define UNIT_SKILLS          0x33  // 5 equipped skills, 2 bytes apart
#define UNIT_FLAGS_SIZE      0x2A
#define FLAG_PURE_WATER      0x1A  // Res bonus amount
#define FLAG_RALLY_FIRST     0x1C  // 0x1C..0x26: rally / map effects
#define FLAG_TONICS          0x27  // bit i = tonic for stat i (+5 HP, +2 others)
#define FLAG_BARRACKS        0x28  // bit i = barracks boost for stat i (+4, not HP)
#define LEARNED_SIZE         0x0D  // learned-skill bit list, right after the flags block
#define LOG_NAME_SIZE        0x1A
#define LOG_ASSET            0x1A
#define LOG_FLAW             0x1B

// USER block layout
#define USER_BASE_SIZE       0xEE
#define USER_CHAPTER_SIZE    0x10
#define USER_CHAPTER_COUNT   0x15  // 5-byte header + 0x10 (US/EU)
#define USER_GOLD_IN_TAIL    0x69

#define SKILL_LIMIT_BREAKER  91

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool parse_layout(fe_save *s, char *err, size_t errlen);

static bool block_ok(const fe_save *s, uint32_t off, const uint8_t magic[4])
{
    return off >= s->hdr_size && off + 5 <= s->size && memcmp(s->data + off, magic, 4) == 0;
}

//---------------------------------------------------------------------------
// Loading
//---------------------------------------------------------------------------
static bool parse_user(fe_save *s, char *err, size_t errlen)
{
    const uint8_t *ub = s->data + s->user_off;
    uint32_t ulen = s->user_end - s->user_off;

    if (ulen < USER_BASE_SIZE || (ulen - USER_BASE_SIZE) % USER_CHAPTER_SIZE != 0)
        return fail(err, errlen, "USER block has unexpected size 0x%X", (unsigned)ulen);

    uint32_t chapters = (ulen - USER_BASE_SIZE) / USER_CHAPTER_SIZE;
    if (ub[USER_CHAPTER_COUNT] != chapters)
        return fail(err, errlen, "USER block chapter count mismatch (%u vs %u). "
                    "Japanese saves are not supported yet.", ub[USER_CHAPTER_COUNT], (unsigned)chapters);

    uint32_t gold_rel = USER_CHAPTER_COUNT + 1 + chapters * USER_CHAPTER_SIZE + USER_GOLD_IN_TAIL;
    if (gold_rel + 8 > ulen)  // gold + renown
        return fail(err, errlen, "gold offset 0x%X outside USER block", (unsigned)gold_rel);

    s->gold_off = s->user_off + gold_rel;
    return true;
}

static bool parse_units(fe_save *s, char *err, size_t errlen)
{
    const uint8_t *d = s->data;
    uint32_t p = s->unit_off + 5;
    const uint32_t end = s->unit_end;

    s->unit_count = 0;
    for (uint8_t g = 0; g < FE_MAX_GROUPS; g++) {
        if (p + 2 > end || d[p] != g)
            continue;  // empty groups are omitted
        uint32_t count = d[p + 1];
        p += 2;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t start = p;
            if (p + UNIT_SUPPORT_LEN_OFF + 1 > end)
                return fail(err, errlen, "unit %u of group %u is truncated", (unsigned)i, g);
            uint32_t sup = d[p + UNIT_SUPPORT_LEN_OFF];
            uint32_t unk_off = p + UNIT_SUPPORT_LEN_OFF + 1 + sup;
            if (unk_off >= end)
                return fail(err, errlen, "unit %u of group %u: bad support length", (unsigned)i, g);
            uint32_t unk = d[unk_off];

            uint32_t flags_off = unk_off + 1 + unk;
            uint32_t q = p + UNIT_FIXED_SIZE + sup + unk;
            if (flags_off + UNIT_FLAGS_SIZE + LEARNED_SIZE > q)
                return fail(err, errlen, "unit %u of group %u: flags block out of range", (unsigned)i, g);
            if (q > end)
                return fail(err, errlen, "unit %u of group %u runs past UNIT block", (unsigned)i, g);

            int32_t log_off = -1, child_off = -1;
            if (d[q - 2] == 1 && d[q - 1] == 6) {
                log_off = (int32_t)q;
                q += UNIT_LOG_SIZE_US;
                if (q > end)
                    return fail(err, errlen, "unit %u of group %u: logbook data truncated", (unsigned)i, g);
                if (d[q - 1] == 1) {
                    child_off = (int32_t)q;
                    q += UNIT_CHILD_SIZE;
                }
            } else if (d[q - 2] == 0 && d[q - 1] == 1) {
                child_off = (int32_t)q;
                q += UNIT_CHILD_SIZE;
            }
            if (q > end)
                return fail(err, errlen, "unit %u of group %u: child data truncated", (unsigned)i, g);

            if (s->unit_count >= FE_MAX_UNITS)
                return fail(err, errlen, "too many units");
            fe_unit *u = &s->units[s->unit_count++];
            u->off = start;
            u->size = q - start;
            u->group = g;
            u->log_off = log_off;
            u->child_off = child_off;
            u->flags_off = flags_off;
            p = q;
        }
    }

    // In every save examined the unit list is followed by exactly one 0xFF byte.
    // Anything else means the walk is misaligned, so refuse rather than guess.
    if (p + 1 != end || d[p] != 0xFF)
        return fail(err, errlen, "unexpected data after unit list (%u bytes left). "
                    "Save layout not recognised.", (unsigned)(end - p));
    return true;
}

bool fe_save_load(fe_save *s, const uint8_t *file, size_t len, char *err, size_t errlen)
{
    memset(s, 0, sizeof(*s));

    if (len >= 0x80 + COMP_HDR_SIZE && memcmp(file + 0x80, MAGIC_COMP, 4) == 0)
        return fail(err, errlen, "this is a Japanese save; only US/EU saves are supported for now");
    if (len < FE_HEADER_US + COMP_HDR_SIZE + 8 || memcmp(file + FE_HEADER_US, MAGIC_COMP, 4) != 0)
        return fail(err, errlen, "not a compressed Awakening Chapter/Map save (no COMP header)");

    const uint8_t *comp = file + FE_HEADER_US;
    uint32_t type = rd32(comp + 4);
    uint32_t declen = rd32(comp + 8);
    uint32_t crc_stored = rd32(comp + 12);

    if (type != COMP_TYPE_HUFFMAN8)
        return fail(err, errlen, "unknown compression type %u", (unsigned)type);
    if (declen < 0x100 || declen > MAX_DECOMPRESSED)
        return fail(err, errlen, "implausible decompressed size %u", (unsigned)declen);

    s->hdr_size = FE_HEADER_US;
    s->size = FE_HEADER_US + declen;
    s->data = malloc(s->size);
    if (!s->data)
        return fail(err, errlen, "out of memory (%u bytes)", (unsigned)s->size);
    memcpy(s->data, file, FE_HEADER_US);

    huff_result hr = huff_decompress(comp + COMP_HDR_SIZE, len - FE_HEADER_US - COMP_HDR_SIZE,
                                     s->data + FE_HEADER_US, declen);
    if (hr != HUFF_OK) {
        fail(err, errlen, "decompression failed: %s", huff_strerror(hr));
        goto bad;
    }

    uint32_t crc = fe_crc32(0, s->data, s->size);
    if (crc != crc_stored) {
        fail(err, errlen, "checksum mismatch (stored %08X, computed %08X); save may be corrupt",
             (unsigned)crc_stored, (unsigned)crc);
        goto bad;
    }

    if (!parse_layout(s, err, errlen))
        goto bad;
    return true;

bad:
    fe_save_free(s);
    return false;
}

// Reads the INDE index and parses USER/UNIT. Used on load and after a resize.
static bool parse_layout(fe_save *s, char *err, size_t errlen)
{
    if (memcmp(s->data + s->hdr_size, MAGIC_INDE, 4) != 0)
        return fail(err, errlen, "block index (INDE) not found");

    uint32_t idx[9];
    for (int i = 0; i < 9; i++)
        idx[i] = rd32(s->data + s->hdr_size + 4 + 4 * i);

    // Map files have two extra blocks (PERS first, MAP after TRAN)
    s->is_map = idx[8] != 0;
    int b = s->is_map ? 1 : 0;
    s->user_off = idx[b + 0];
    uint32_t gmap = idx[b + 1];
    s->unit_off = idx[b + 2];
    uint32_t refi = idx[b + 3];
    s->user_end = gmap;
    s->unit_end = refi;

    if (!block_ok(s, s->user_off, MAGIC_USER) || !block_ok(s, gmap, MAGIC_GMAP) ||
        !block_ok(s, s->unit_off, MAGIC_UNIT) || !block_ok(s, refi, MAGIC_REFI) ||
        !(s->user_off < gmap && gmap < s->unit_off && s->unit_off < refi))
        return fail(err, errlen, "block index does not match the expected USER/GMAP/UNIT/REFI layout");

    // Every block the index points at must start with a 4-letter magic, in order.
    uint32_t prev = s->hdr_size;
    for (int i = 0; i < 9; i++) {
        if (!idx[i])
            continue;
        if (idx[i] <= prev || idx[i] + 4 > s->size)
            return fail(err, errlen, "block index entry %d (0x%X) out of order", i, (unsigned)idx[i]);
        for (int k = 0; k < 4; k++) {
            uint8_t ch = s->data[idx[i] + k];
            if (ch < 0x20 || ch > 0x7E)
                return fail(err, errlen, "block index entry %d does not point at a block", i);
        }
        prev = idx[i];
    }

    // Forged weapons and convoy (REFI and TRAN follow UNIT)
    uint32_t tran = idx[b + 4];
    uint32_t tran_end = idx[b + 5];
    if (!block_ok(s, tran, MAGIC_TRAN) || tran <= refi || !tran_end || tran_end <= tran)
        return fail(err, errlen, "convoy (TRAN) block not found where expected");
    // World map: only used if it looks exactly as documented
    s->gmap_off = 0;
    s->map_count = 0;
    if (gmap + 0x3E <= s->unit_off && memcmp(s->data + gmap, "PAMG", 4) == 0) {
        int n = s->data[gmap + 0x3D];
        if (gmap + 0x3E + (uint32_t)n * 0x1D <= s->unit_off) {
            s->gmap_off = gmap;
            s->map_count = n;
        }
    }
    s->refi_off = refi;
    s->forge_count = s->data[refi + 5];
    if (refi + 7 + (uint32_t)s->forge_count * 0x2C > tran)
        return fail(err, errlen, "forged weapon list runs past its block");
    s->tran_off = tran;
    s->convoy_count = s->data[tran + 5] | (s->data[tran + 6] << 8);
    if (tran + 7 + (uint32_t)s->convoy_count * 2 > tran_end || s->convoy_count < FE_ITEM_COUNT)
        return fail(err, errlen, "convoy table size %d does not fit its block", s->convoy_count);

    return parse_user(s, err, errlen) && parse_units(s, err, errlen);
}

void fe_save_free(fe_save *s)
{
    free(s->data);
    s->data = NULL;
    s->size = 0;
    s->unit_count = 0;
}

//---------------------------------------------------------------------------
// Rebuilding
//---------------------------------------------------------------------------
bool fe_save_build(const fe_save *s, uint8_t **out, size_t *out_len, char *err, size_t errlen)
{
    *out = NULL;
    *out_len = 0;

    const uint8_t *payload = s->data + s->hdr_size;
    const uint32_t declen = s->size - s->hdr_size;

    uint8_t *comp = NULL;
    size_t comp_len = 0;
    huff_result hr = huff_compress(payload, declen, &comp, &comp_len);
    if (hr != HUFF_OK)
        return fail(err, errlen, "compression failed: %s", huff_strerror(hr));

    // Verification 1: the compressed stream must decode back to exactly the payload.
    uint8_t *check = malloc(declen);
    if (!check) {
        free(comp);
        return fail(err, errlen, "out of memory during verification");
    }
    hr = huff_decompress(comp, comp_len, check, declen);
    bool same = (hr == HUFF_OK) && memcmp(check, payload, declen) == 0;
    free(check);
    if (!same) {
        free(comp);
        return fail(err, errlen, "verification failed: recompressed data does not decode back (%s)",
                    hr == HUFF_OK ? "content differs" : huff_strerror(hr));
    }

    size_t total = s->hdr_size + COMP_HDR_SIZE + comp_len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        free(comp);
        return fail(err, errlen, "out of memory (%u bytes)", (unsigned)total);
    }
    memcpy(buf, s->data, s->hdr_size);
    uint8_t *ch = buf + s->hdr_size;
    memcpy(ch, MAGIC_COMP, 4);
    wr32(ch + 4, COMP_TYPE_HUFFMAN8);
    wr32(ch + 8, declen);
    wr32(ch + 12, fe_crc32(0, s->data, s->size));
    memcpy(ch + COMP_HDR_SIZE, comp, comp_len);
    free(comp);

    // Verification 2: the finished file must load through the normal path
    // and produce the identical decompressed save.
    fe_save *v = malloc(sizeof(fe_save));
    if (!v) {
        free(buf);
        return fail(err, errlen, "out of memory during verification");
    }
    char verr[128];
    bool ok = fe_save_load(v, buf, total, verr, sizeof(verr));
    if (ok) {
        ok = v->size == s->size && memcmp(v->data, s->data, s->size) == 0 &&
             v->unit_count == s->unit_count;
        if (!ok)
            snprintf(verr, sizeof(verr), "reloaded save differs from edited save");
    }
    fe_save_free(v);
    free(v);
    if (!ok) {
        free(buf);
        return fail(err, errlen, "verification failed: %s", verr);
    }

    *out = buf;
    *out_len = total;
    return true;
}

//---------------------------------------------------------------------------
// Fields
//---------------------------------------------------------------------------
uint32_t fe_get_gold(const fe_save *s)
{
    return rd32(s->data + s->gold_off);
}

void fe_set_gold(fe_save *s, uint32_t gold)
{
    if (gold > FE_GOLD_MAX)
        gold = FE_GOLD_MAX;
    wr32(s->data + s->gold_off, gold);
}

uint32_t fe_get_renown(const fe_save *s)
{
    return rd32(s->data + s->gold_off + 4);
}

void fe_set_renown(fe_save *s, uint32_t renown)
{
    if (renown > FE_RENOWN_MAX)
        renown = FE_RENOWN_MAX;
    wr32(s->data + s->gold_off + 4, renown);
}

uint16_t fe_unit_char_id(const fe_save *s, const fe_unit *u)
{
    const uint8_t *p = s->data + u->off + UNIT_CHAR_ID;
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint8_t fe_unit_class_id(const fe_save *s, const fe_unit *u)
{
    return s->data[u->off + UNIT_CLASS];
}

uint8_t fe_unit_level(const fe_save *s, const fe_unit *u)
{
    return s->data[u->off + UNIT_LEVEL];
}

uint8_t fe_unit_exp(const fe_save *s, const fe_unit *u)
{
    return s->data[u->off + UNIT_EXP];
}

// First forged-weapon id: the convoy table has one entry per item id, regular items first and
// then the forge slots, so this follows the save (vanilla 202, Thabes' mod 306) even without mod files.
static int forge_base_id(const fe_save *s)
{
    return s->convoy_count >= FE_FORGE_MAX ? s->convoy_count - FE_FORGE_MAX : fe_item_count();
}

static const char *char_name(int id)
{
    return fe_char(id) ? fe_char(id)->name : "?";
}

bool fe_save_is_modded(const fe_save *s)
{
    if (s->convoy_count != FE_ITEM_COUNT + FE_FORGE_MAX)
        return true;
    for (int i = 0; i < s->unit_count; i++)
        if (fe_unit_char_id(s, &s->units[i]) >= FE_CHAR_COUNT)
            return true;
    return false;
}

const char *fe_class_name(uint8_t class_id)
{
    const fe_class_info *c = fe_class(class_id);
    return c ? c->name : "Unknown class";
}

const char *fe_group_name(uint8_t group)
{
    static const char *const names[FE_MAX_GROUPS] = {
        "Deployed", "Enemy", "Other", "Army", "Fallen", "Other"};
    return group < FE_MAX_GROUPS ? names[group] : "?";
}

void fe_unit_name(const fe_save *s, const fe_unit *u, char *out, size_t outlen)
{
    if (!outlen)
        return;
    out[0] = '\0';

    if (u->log_off >= 0) {
        // UTF-16LE custom name, BMP only; convert to UTF-8
        const uint8_t *n = s->data + u->log_off;
        size_t o = 0;
        for (int i = 0; i + 1 < LOG_NAME_SIZE; i += 2) {
            uint16_t c = (uint16_t)(n[i] | (n[i + 1] << 8));
            if (c == 0)
                break;
            if (c < 0x80 && o + 1 < outlen) {
                out[o++] = (char)c;
            } else if (c < 0x800 && o + 2 < outlen) {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            } else if (c >= 0x800 && o + 3 < outlen) {
                out[o++] = (char)(0xE0 | (c >> 12));
                out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        out[o] = '\0';
        if (o > 0)
            return;
    }

    uint16_t id = fe_unit_char_id(s, u);
    if (fe_char(id))
        snprintf(out, outlen, "%s", fe_char(id)->name);
    else
        snprintf(out, outlen, "Character #%u", id);
}

int fe_unit_level_cap(const fe_save *s, const fe_unit *u)
{
    // Special classes with a level cap of 30 (same list as FireEditor):
    // Dancer, Manakete, Taguel M/F, Villager, Lodestar, Grima, Dread Fighter, Bride
    static const uint8_t cap30[] = {67, 68, 69, 70, 72, 77, 78, 80, 81};
    uint8_t c = fe_unit_class_id(s, u);
    for (size_t i = 0; i < sizeof(cap30); i++)
        if (cap30[i] == c)
            return 30;
    return 20;
}

int fe_unit_set_level(fe_save *s, const fe_unit *u, int level)
{
    int cap = fe_unit_level_cap(s, u);
    if (level < 1) level = 1;
    if (level > cap) level = cap;
    s->data[u->off + UNIT_LEVEL] = (uint8_t)level;
    if (level == cap)
        s->data[u->off + UNIT_EXP] = 0;
    return level;
}

//---------------------------------------------------------------------------
// Stats (ported from FireEditor's Stats.java)
//---------------------------------------------------------------------------
static const int8_t ASSET_MODS[9][FE_STAT_COUNT] = {
    {0, 0, 0, 0, 0, 0, 0, 0},  // none
    {0, 1, 1, 0, 0, 2, 2, 2},  // HP
    {0, 4, 0, 2, 0, 0, 2, 0},  // Str
    {0, 0, 4, 0, 2, 0, 0, 2},  // Mag
    {0, 2, 0, 4, 0, 0, 2, 0},  // Skl
    {0, 0, 0, 2, 4, 2, 0, 0},  // Spd
    {0, 2, 2, 0, 0, 4, 0, 0},  // Lck
    {0, 0, 0, 0, 0, 2, 4, 2},  // Def
    {0, 0, 2, 0, 2, 0, 0, 4},  // Res
};
static const int8_t FLAW_MODS[9][FE_STAT_COUNT] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 1},
    {0, 3, 0, 1, 0, 0, 1, 0},
    {0, 0, 3, 0, 1, 0, 0, 1},
    {0, 1, 0, 3, 0, 0, 1, 0},
    {0, 0, 0, 1, 3, 1, 0, 0},
    {0, 1, 1, 0, 0, 3, 0, 0},
    {0, 0, 0, 0, 0, 1, 3, 1},
    {0, 0, 1, 0, 1, 0, 0, 3},
};

const char *const fe_stat_names[FE_STAT_COUNT] = {"HP", "Str", "Mag", "Skl", "Spd", "Lck", "Def", "Res"};

static void add_asset_flaw(int mods[FE_STAT_COUNT], int asset, int flaw)
{
    if (asset < 0 || asset > 8) asset = 0;
    if (flaw < 0 || flaw > 8) flaw = 0;
    for (int i = 0; i < FE_STAT_COUNT; i++)
        mods[i] += ASSET_MODS[asset][i] - FLAW_MODS[flaw][i];
}

static void add_char_mods(int mods[FE_STAT_COUNT], int id)
{
    for (int i = 0; i < FE_STAT_COUNT; i++)
        mods[i] += fe_char(id) ? fe_char(id)->modifiers[i] : 0;
}

static bool valid_parent(int id)
{
    return id >= 0 && id <= 0x38;
}

// Child block: two 0x11-byte parent records (father, mother) at +1 and +0x12.
// Each holds 3 entries of 5 bytes starting at +2: u16 id, u8 asset, u8 flaw, u8 extra.
// Entry 0 = the parent, 1 = their father, 2 = their mother.
static void child_entry(const uint8_t *child, int parent, int entry, int *id, int *asset, int *flaw)
{
    const uint8_t *e = child + (parent ? 0x12 : 0x01) + 2 + entry * 5;
    *id = e[0] | (e[1] << 8);
    *asset = e[2];
    *flaw = e[3];
}

// Child cap modifiers. FireEditor's version has three copy-paste slips
// (wrong flaw index twice, mother's father counted twice); this uses the
// evidently intended symmetric formula. Flagged for hardware verification.
static void child_mods(const fe_save *s, const fe_unit *u, int mods[FE_STAT_COUNT])
{
    const uint8_t *c = s->data + u->child_off;
    int fid, fa, ff, mid, ma, mf;
    child_entry(c, 0, 0, &fid, &fa, &ff);
    child_entry(c, 1, 0, &mid, &ma, &mf);

    add_char_mods(mods, fe_unit_char_id(s, u));
    if (valid_parent(fid) && valid_parent(mid)) {
        add_char_mods(mods, fid);
        add_char_mods(mods, mid);
        add_asset_flaw(mods, fa, ff);
        add_asset_flaw(mods, ma, mf);

        for (int side = 0; side < 2; side++) {
            int gp, gpa, gpf, gm, gma, gmf;
            child_entry(c, side, 1, &gp, &gpa, &gpf);
            child_entry(c, side, 2, &gm, &gma, &gmf);
            if (valid_parent(gp) && valid_parent(gm)) {
                add_char_mods(mods, gp);
                add_char_mods(mods, gm);
                add_asset_flaw(mods, gpa, gpf);
                add_asset_flaw(mods, gma, gmf);
            }
        }
        for (int i = 0; i < FE_STAT_COUNT; i++)
            mods[i] += 1;  // hardcoded +1 when both parents are valid
    }
    mods[0] = 0;  // HP has no modifier
}

// Temporary and skill bonuses the game adds on top of the permanent stat
// (ported from FireEditor's temporalBuffs/skillBuffs; weapon bonuses not included).
static void unit_buffs(const fe_save *s, const fe_unit *u, int buff[FE_STAT_COUNT])
{
    const uint8_t *f = s->data + u->flags_off;
    const uint8_t *rec = s->data + u->off;
    memset(buff, 0, sizeof(int) * FE_STAT_COUNT);

    buff[7] += f[FLAG_PURE_WATER];
    if (f[0x1C]) { buff[1] += 2; buff[2] += 2; buff[6] += 2; buff[7] += 2; }
    if (f[0x1D]) buff[1] += 4;
    if (f[0x1E]) buff[2] += 4;
    if (f[0x1F]) buff[3] += 4;
    if (f[0x20]) buff[4] += 4;
    if (f[0x21]) buff[5] += 8;
    if (f[0x22]) buff[6] += 4;
    if (f[0x23]) buff[7] += 4;
    if (f[0x24]) for (int i = 1; i < FE_STAT_COUNT; i++) buff[i] += 4;
    if (f[0x26]) for (int i = 1; i < FE_STAT_COUNT; i++) buff[i] += 2;
    for (int i = 0; i < FE_STAT_COUNT; i++) {
        if (f[FLAG_TONICS] & (1 << i)) buff[i] += i == 0 ? 5 : 2;
        if (i > 0 && (f[FLAG_BARRACKS] & (1 << i))) buff[i] += 4;
    }

    // Equipped stat skills (each counts once): HP+5, Str/Mag/Skl/Spd/Def/Res +2,
    // Luck +4 (53), Res +10 (99), all +2 (88)
    uint8_t seen[5];
    for (int k = 0; k < 5; k++) {
        uint8_t sk = rec[UNIT_SKILLS + k * 2];
        bool dup = false;
        for (int j = 0; j < k; j++) dup |= seen[j] == sk;
        seen[k] = sk;
        if (dup) continue;
        switch (sk) {
        case 1: buff[0] += 5; break;
        case 2: buff[1] += 2; break;
        case 3: buff[2] += 2; break;
        case 4: buff[3] += 2; break;
        case 5: buff[4] += 2; break;
        case 6: buff[6] += 2; break;
        case 7: buff[7] += 2; break;
        case 53: buff[5] += 4; break;
        case 99: buff[7] += 10; break;
        case 88: for (int i = 1; i < FE_STAT_COUNT; i++) buff[i] += 2; break;
        default: break;
        }
    }
}

bool fe_unit_stats(const fe_save *s, const fe_unit *u, fe_stats *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t id = fe_unit_char_id(s, u);
    uint8_t cls = fe_unit_class_id(s, u);
    const fe_char_info *ch = fe_char(id);
    const fe_class_info *cl = fe_class(cls);
    if (!ch || !cl)
        return false;

    const uint8_t *rec = s->data + u->off;
    int asset = 0, flaw = 0;
    if (u->log_off >= 0) {
        asset = s->data[u->log_off + LOG_ASSET];
        flaw = s->data[u->log_off + LOG_FLAW];
        if (asset > 8) asset = 0;
        if (flaw > 8) flaw = 0;
    }

    // Personal base, adjusted by the Avatar's asset/flaw. Values from the game's
    // own table (fireemblemwiki "Robin/Stats"): HP +5/-3, Luck +4/-2, others +2/-1.
    // FireEditor used +2/-1 for HP and +2 for a Luck asset, which hardware testing
    // showed is wrong (HP-flaw Robin displayed 2 HP too high).
    static const int8_t ASSET_BASE[9] = {0, 5, 2, 2, 2, 2, 4, 2, 2};
    static const int8_t FLAW_BASE[9]  = {0, 3, 1, 1, 1, 1, 2, 1, 1};
    int add[FE_STAT_COUNT];
    for (int i = 0; i < FE_STAT_COUNT; i++)
        add[i] = ch->additions[i];
    if (asset)
        add[asset - 1] += ASSET_BASE[asset];
    if (flaw)
        add[flaw - 1] -= FLAW_BASE[flaw];

    // Cap modifiers
    int mods[FE_STAT_COUNT] = {0};
    if (u->child_off >= 0) {
        child_mods(s, u, mods);
        if (u->log_off >= 0)
            add_asset_flaw(mods, asset, flaw);
    } else {
        add_char_mods(mods, id);
        if (u->log_off >= 0)
            add_asset_flaw(mods, asset, flaw);
    }

    for (int k = 0; k < 5; k++)
        if (rec[UNIT_SKILLS + k * 2] == SKILL_LIMIT_BREAKER)
            out->limit_breaker = true;
    unit_buffs(s, u, out->buff);

    for (int i = 0; i < FE_STAT_COUNT; i++) {
        out->base[i] = add[i] + cl->base[i];
        out->cap[i] = cl->max[i] + mods[i] + ((out->limit_breaker && i) ? 10 : 0);

        int v = out->base[i] + rec[UNIT_GAINS + i];
        if (v > 255)
            v -= 256;  // gains wrap as 8-bit (seen on some enemy logbook units)
        else if (v > out->cap[i])
            v = out->cap[i];
        out->value[i] = v;
    }
    return true;
}

int fe_unit_set_stat(fe_save *s, const fe_unit *u, int stat, int value)
{
    if (stat < 0 || stat >= FE_STAT_COUNT)
        return -1;
    fe_stats st;
    if (!fe_unit_stats(s, u, &st))
        return -1;

    int lo = st.base[stat] < 0 ? 0 : st.base[stat];
    int hi = st.cap[stat];
    if (hi < lo) hi = lo;
    if (value < lo) value = lo;
    if (value > hi) value = hi;

    int gain = value - st.base[stat];
    if (gain < 0) gain = 0;
    if (gain > 255) gain = 255;

    uint8_t *rec = s->data + u->off;
    rec[UNIT_GAINS + stat] = (uint8_t)gain;

    if (stat == 0) {
        // Keep current HP sensible: full-health units stay at full health,
        // and nobody ends up above their new max.
        int cur = rec[UNIT_CUR_HP];
        int old_max = st.value[0] + st.buff[0], new_max = value + st.buff[0];
        if (cur >= old_max || cur > new_max)
            rec[UNIT_CUR_HP] = (uint8_t)(new_max > 255 ? 255 : new_max);
    }

    fe_unit_stats(s, u, &st);
    return st.value[stat];
}

//---------------------------------------------------------------------------
// Skills
//---------------------------------------------------------------------------
static uint8_t *learned_bits(fe_save *s, const fe_unit *u)
{
    return s->data + u->flags_off + UNIT_FLAGS_SIZE;
}

static bool valid_skill(int skill)
{
    return skill >= FE_SKILL_FIRST && skill <= FE_SKILL_LAST;
}

void fe_unit_equipped(const fe_save *s, const fe_unit *u, int out[FE_EQUIP_SLOTS])
{
    const uint8_t *rec = s->data + u->off;
    for (int k = 0; k < FE_EQUIP_SLOTS; k++)
        out[k] = rec[UNIT_SKILLS + k * 2] | (rec[UNIT_SKILLS + k * 2 + 1] << 8);
}

bool fe_unit_is_equipped(const fe_save *s, const fe_unit *u, int skill)
{
    int eq[FE_EQUIP_SLOTS];
    fe_unit_equipped(s, u, eq);
    for (int k = 0; k < FE_EQUIP_SLOTS; k++)
        if (skill && eq[k] == skill)
            return true;
    return false;
}

bool fe_unit_is_learned(const fe_save *s, const fe_unit *u, int skill)
{
    if (skill < 0 || skill >= FE_SKILL_COUNT)
        return false;
    const uint8_t *bits = s->data + u->flags_off + UNIT_FLAGS_SIZE;
    return (bits[skill / 8] >> (skill % 8)) & 1;
}

int fe_unit_learned_count(const fe_save *s, const fe_unit *u)
{
    int n = 0;
    for (int id = FE_SKILL_FIRST; id <= FE_SKILL_LAST; id++)
        n += fe_unit_is_learned(s, u, id);
    return n;
}

// Writes the slots back with filled ones first, high bytes zero.
static void write_equipped(fe_save *s, const fe_unit *u, const int eq[FE_EQUIP_SLOTS])
{
    uint8_t *rec = s->data + u->off;
    int k = 0;
    for (int i = 0; i < FE_EQUIP_SLOTS; i++) {
        if (eq[i]) {
            rec[UNIT_SKILLS + k * 2] = (uint8_t)eq[i];
            rec[UNIT_SKILLS + k * 2 + 1] = 0;
            k++;
        }
    }
    for (; k < FE_EQUIP_SLOTS; k++) {
        rec[UNIT_SKILLS + k * 2] = 0;
        rec[UNIT_SKILLS + k * 2 + 1] = 0;
    }
}

// Equipping/unequipping HP +5 changes max HP; keep current HP consistent the
// same way stat edits do (full stays full, never above max).
static int shown_max_hp(const fe_save *s, const fe_unit *u)
{
    fe_stats st;
    return fe_unit_stats(s, u, &st) ? fe_stat_shown(&st, 0) : -1;
}

static void fix_current_hp(fe_save *s, const fe_unit *u, int old_max)
{
    int new_max = shown_max_hp(s, u);
    if (old_max < 0 || new_max < 0 || new_max == old_max)
        return;
    uint8_t *cur = s->data + u->off + UNIT_CUR_HP;
    if (*cur >= old_max || *cur > new_max)
        *cur = (uint8_t)(new_max > 255 ? 255 : new_max);
}

void fe_unit_unequip(fe_save *s, const fe_unit *u, int skill)
{
    if (!skill)
        return;
    int old_max = shown_max_hp(s, u);
    int eq[FE_EQUIP_SLOTS];
    fe_unit_equipped(s, u, eq);
    for (int k = 0; k < FE_EQUIP_SLOTS; k++)
        if (eq[k] == skill)
            eq[k] = 0;
    write_equipped(s, u, eq);
    fix_current_hp(s, u, old_max);
}

int fe_unit_equip(fe_save *s, const fe_unit *u, int skill)
{
    if (!valid_skill(skill) || fe_unit_is_equipped(s, u, skill))
        return -1;
    int eq[FE_EQUIP_SLOTS];
    fe_unit_equipped(s, u, eq);

    int n = 0;
    for (int k = 0; k < FE_EQUIP_SLOTS; k++)
        if (eq[k]) eq[n++] = eq[k];
    if (n >= FE_EQUIP_SLOTS)
        return -1;
    for (int k = n; k < FE_EQUIP_SLOTS; k++)
        eq[k] = 0;

    int old_max = shown_max_hp(s, u);
    fe_unit_set_learned(s, u, skill, true);
    eq[n] = skill;
    write_equipped(s, u, eq);
    fix_current_hp(s, u, old_max);
    return n;
}

void fe_unit_set_learned(fe_save *s, const fe_unit *u, int skill, bool learned)
{
    if (!valid_skill(skill))
        return;
    if (!learned && fe_unit_is_equipped(s, u, skill))
        fe_unit_unequip(s, u, skill);
    uint8_t *bits = learned_bits(s, u);
    if (learned)
        bits[skill / 8] |= (uint8_t)(1 << (skill % 8));
    else
        bits[skill / 8] &= (uint8_t)~(1 << (skill % 8));
}

int fe_unit_learn_all(fe_save *s, const fe_unit *u)
{
    int added = 0;
    for (int id = FE_SKILL_FIRST; id <= FE_SKILL_LAST; id++) {
        if (!fe_unit_is_learned(s, u, id)) {
            fe_unit_set_learned(s, u, id, true);
            added++;
        }
    }
    return added;
}

//---------------------------------------------------------------------------
// Class
//---------------------------------------------------------------------------
bool fe_unit_is_female(const fe_save *s, const fe_unit *u)
{
    uint8_t c = fe_unit_class_id(s, u);
    return fe_class(c) && fe_class(c)->female;
}

bool fe_unit_set_class(fe_save *s, const fe_unit *u, int class_id)
{
    if (!fe_class(class_id))
        return false;
    int old_max = shown_max_hp(s, u);
    s->data[u->off + UNIT_CLASS] = (uint8_t)class_id;

    int cap = fe_unit_level_cap(s, u);
    if (fe_unit_level(s, u) >= cap)
        fe_unit_set_level(s, u, cap);

    fix_current_hp(s, u, old_max);
    return true;
}

//---------------------------------------------------------------------------
// Supports
//---------------------------------------------------------------------------
static int support_slot(const fe_save *s, const fe_unit *u, int partner_char)
{
    uint16_t id = fe_unit_char_id(s, u);
    const fe_support_list *l = fe_char_supports(id);
    if (!l || partner_char < 0 || partner_char == FE_SUPPORT_EMPTY)
        return -1;
    for (int k = 0; k < l->count; k++)
        if (l->partners[k].partner == partner_char)
            return k;
    return -1;
}

int fe_support_get(const fe_save *s, const fe_unit *u, int partner_char)
{
    int k = support_slot(s, u, partner_char);
    if (k < 0)
        return -1;
    const uint8_t *rec = s->data + u->off;
    return k < rec[UNIT_SUPPORT_LEN_OFF] ? rec[UNIT_SUPPORT_LEN_OFF + 1 + k] : 0;
}

int fe_find_unit(const fe_save *s, int char_id)
{
    static const uint8_t order[] = {FE_GROUP_ARMY, FE_GROUP_BLUE, FE_GROUP_DEAD};
    for (size_t g = 0; g < sizeof(order); g++)
        for (int i = 0; i < s->unit_count; i++)
            if (s->units[i].group == order[g] && fe_unit_char_id(s, &s->units[i]) == char_id)
                return i;
    return -1;
}

// Inserts `len` bytes at `pos` (from `ins`, or zeros if NULL), applies byte patches at
// offsets before `pos`, shifts every block-index entry at or after `pos`, and re-parses
// the result on a copy. *s is replaced only if the new layout parses and has
// `expect_units` units.
// Replaces `old_len` bytes at `pos` with `len` new ones; see splice_save.
static bool splice_replace(fe_save *s, uint32_t pos, uint32_t old_len, const uint8_t *ins, uint32_t len,
                           const uint32_t *patch_off, const uint8_t *patch_val, int npatch,
                           int expect_units, char *err, size_t errlen)
{
    uint32_t new_size = s->size - old_len + len;
    uint8_t *buf = malloc(new_size);
    if (!buf)
        return fail(err, errlen, "out of memory growing the save");
    memcpy(buf, s->data, pos);
    if (ins) memcpy(buf + pos, ins, len);
    else memset(buf + pos, 0, len);
    memcpy(buf + pos + len, s->data + pos + old_len, s->size - pos - old_len);
    for (int i = 0; i < npatch; i++)
        buf[patch_off[i]] = patch_val[i];
    for (int i = 0; i < 9; i++) {
        uint32_t at = s->hdr_size + 4 + 4 * (uint32_t)i;
        uint32_t v = rd32(buf + at);
        if (v && v >= pos + old_len)
            wr32(buf + at, v - old_len + len);
    }

    fe_save *t = malloc(sizeof(fe_save));
    if (!t) {
        free(buf);
        return fail(err, errlen, "out of memory growing the save");
    }
    memset(t, 0, sizeof(*t));
    t->data = buf;
    t->size = new_size;
    t->hdr_size = s->hdr_size;
    char perr[160];
    if (!parse_layout(t, perr, sizeof(perr)) || t->unit_count != expect_units) {
        free(buf);
        free(t);
        return fail(err, errlen, "grown save failed to re-parse: %s", perr);
    }
    free(s->data);
    *s = *t;
    free(t);
    return true;
}

static bool splice_save(fe_save *s, uint32_t pos, const uint8_t *ins, uint32_t len,
                        const uint32_t *patch_off, const uint8_t *patch_val, int npatch,
                        int expect_units, char *err, size_t errlen)
{
    return splice_replace(s, pos, 0, ins, len, patch_off, patch_val, npatch, expect_units, err, errlen);
}

// Grows unit `ui`'s support block to at least `need` bytes (new bytes = 0).
static bool grow_support_block(fe_save *s, int ui, int need, char *err, size_t errlen)
{
    const fe_unit *u = &s->units[ui];
    uint32_t len_off = u->off + UNIT_SUPPORT_LEN_OFF;
    int len = s->data[len_off];
    if (need <= len)
        return true;
    if (need > 255)
        return fail(err, errlen, "support block would exceed 255 bytes");
    uint8_t v = (uint8_t)need;
    return splice_save(s, len_off + 1 + (uint32_t)len, NULL, (uint32_t)(need - len), &len_off, &v, 1,
                       s->unit_count, err, errlen);
}

static bool set_one_side(fe_save *s, int ui, int partner_char, int value, char *err, size_t errlen)
{
    int k = support_slot(s, &s->units[ui], partner_char);
    if (k < 0)
        return true;  // partner doesn't list this unit (one-sided pair); nothing to store
    if (s->data[s->units[ui].off + UNIT_SUPPORT_LEN_OFF] <= k && value == 0)
        return true;  // already 0 by omission
    if (!grow_support_block(s, ui, k + 1, err, errlen))
        return false;
    s->data[s->units[ui].off + UNIT_SUPPORT_LEN_OFF + 1 + k] = (uint8_t)value;
    return true;
}

bool fe_support_set(fe_save *s, int unit_index, int partner_char, int value, char *err, size_t errlen)
{
    if (unit_index < 0 || unit_index >= s->unit_count)
        return fail(err, errlen, "invalid unit");
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    if (support_slot(s, &s->units[unit_index], partner_char) < 0)
        return fail(err, errlen, "not a support partner of this unit");

    // Work on a copy so a failure on the partner side can't leave a half-edited pair
    uint8_t *backup = malloc(s->size);
    if (!backup)
        return fail(err, errlen, "out of memory");
    memcpy(backup, s->data, s->size);
    uint32_t backup_size = s->size;
    int self_char = fe_unit_char_id(s, &s->units[unit_index]);

    bool ok = set_one_side(s, unit_index, partner_char, value, err, errlen);
    int pi = ok ? fe_find_unit(s, partner_char) : -1;
    if (ok && pi >= 0)
        ok = set_one_side(s, pi, self_char, value, err, errlen);

    if (!ok) {
        // Restore the original buffer and layout
        free(s->data);
        s->data = backup;
        s->size = backup_size;
        char perr[64];
        parse_layout(s, perr, sizeof(perr));
        return false;
    }
    free(backup);
    return true;
}

static const int SUPPORT_READY[5][4] = {
    {3, 9, 17, -1},   // non-romantic: C, B, A
    {4, 9, 15, 21},   // slow
    {3, 8, 13, 19},   // medium
    {2, 7, 12, 17},   // fast
    {-1, -1, -1, -1}, // listed in a romfs mod but no rank can be reached
};
static const char SUPPORT_RANKS[4] = {'C', 'B', 'A', 'S'};

void fe_support_rank_name(int type, int value, char *out, size_t outlen)
{
    if (type < 0 || type > 4) type = 0;
    const int *r = SUPPORT_READY[type];
    const char *rank = "-";
    char buf[16];
    for (int i = 3; i >= 0; i--) {
        if (r[i] < 0) continue;
        if (value == r[i]) { snprintf(buf, sizeof(buf), "%c ready", SUPPORT_RANKS[i]); rank = buf; break; }
        if (value > r[i]) { snprintf(buf, sizeof(buf), "%c", SUPPORT_RANKS[i]); rank = buf; break; }
    }
    snprintf(out, outlen, "%s", rank);
}

int fe_support_next_ready(int type, int value)
{
    if (type < 0 || type > 4) type = 0;
    for (int i = 0; i < 4; i++)
        if (SUPPORT_READY[type][i] > value)
            return SUPPORT_READY[type][i];
    return -1;
}

int fe_support_prev_ready(int type, int value)
{
    if (type < 0 || type > 4) type = 0;
    int best = 0;
    for (int i = 0; i < 4; i++)
        if (SUPPORT_READY[type][i] >= 0 && SUPPORT_READY[type][i] < value)
            best = SUPPORT_READY[type][i];
    return best;
}

//---------------------------------------------------------------------------
// Marriage guard
//---------------------------------------------------------------------------
static int s_ready(int type)
{
    return (type >= 1 && type <= 3) ? SUPPORT_READY[type][3] : -1;
}

int fe_support_spouse(const fe_save *s, int unit_index, int except_char)
{
    const fe_unit *u = &s->units[unit_index];
    uint16_t id = fe_unit_char_id(s, u);
    const fe_support_list *l = fe_char_supports(id);
    if (!l)
        return -1;
    for (int k = 0; k < l->count; k++) {
        int sr = s_ready(l->partners[k].type);
        if (sr < 0 || l->partners[k].partner == except_char || l->partners[k].partner == FE_SUPPORT_EMPTY)
            continue;
        if (fe_support_get(s, u, l->partners[k].partner) > sr)
            return l->partners[k].partner;
    }
    return -1;
}

bool fe_support_s_would_unmarry(const fe_save *s, int unit_index, int partner_char)
{
    int self = fe_unit_char_id(s, &s->units[unit_index]);
    if (fe_support_spouse(s, unit_index, partner_char) >= 0)
        return true;
    int pi = fe_find_unit(s, partner_char);
    return pi >= 0 && fe_support_spouse(s, pi, self) >= 0;
}

int fe_support_fix_pending_s(fe_save *s, char *report, size_t report_len)
{
    int changed = 0;
    size_t o = 0;
    if (report && report_len) report[0] = '\0';

    for (int i = 0; i < s->unit_count; i++) {
        if (fe_find_unit(s, fe_unit_char_id(s, &s->units[i])) != i)
            continue;  // only the player copy of each character
        uint16_t id = fe_unit_char_id(s, &s->units[i]);
        const fe_support_list *l = fe_char_supports(id);
        if (!l)
            continue;
        for (int k = 0; k < l->count; k++) {
            int type = l->partners[k].type, partner = l->partners[k].partner;
            if (partner == FE_SUPPORT_EMPTY)
                continue;
            int sr = s_ready(type);
            if (sr < 0 || fe_support_get(s, &s->units[i], partner) != sr)
                continue;
            if (!fe_support_s_would_unmarry(s, i, partner))
                continue;
            int a_rank = SUPPORT_READY[type][2] + 1;
            char err[96];
            if (!fe_support_set(s, i, partner, a_rank, err, sizeof(err)))
                continue;
            changed++;
            if (report && o < report_len) {
                int spouse = fe_support_spouse(s, i, partner);
                int pi = fe_find_unit(s, partner);
                int pspouse = pi >= 0 ? fe_support_spouse(s, pi, id) : -1;
                int who = spouse >= 0 ? (int)id : partner;
                int to = spouse >= 0 ? spouse : pspouse;
                o += (size_t)snprintf(report + o, report_len - o, "%s - %s: S ready -> A (%s is married to %s)\n",
                                      char_name(id), char_name(partner), char_name(who), char_name(to));
                if (o >= report_len) o = report_len - 1;
            }
        }
    }
    return changed;
}

static int support_steps(int type, int out[9])
{
    if (type < 0 || type > 4) type = 0;
    int n = 0;
    out[n++] = 0;
    for (int i = 0; i < 4; i++) {
        int r = SUPPORT_READY[type][i];
        if (r < 0) break;
        out[n++] = r;      // ready
        out[n++] = r + 1;  // reached
    }
    return n;
}

int fe_support_next_step(int type, int value)
{
    int st[9], n = support_steps(type, st);
    for (int i = 0; i < n; i++)
        if (st[i] > value)
            return st[i];
    return -1;
}

int fe_support_prev_step(int type, int value)
{
    int st[9], n = support_steps(type, st), best = 0;
    for (int i = 0; i < n; i++)
        if (st[i] < value)
            best = st[i];
    return best;
}

//---------------------------------------------------------------------------
// Inventory, forges, convoy
//---------------------------------------------------------------------------
const char *const fe_item_type_names[11] = {
    "Sword", "Lance", "Axe", "Bow", "Tome", "Staff", "Dragonstone", "Beaststone", "Claws", "Breath", "Item"};

#define UNIT_INV 0x1A
#define FORGE_SIZE 0x2C

static uint8_t *inv_slot(fe_save *s, const fe_unit *u, int slot)
{
    return s->data + u->off + UNIT_INV + slot * 5;
}

void fe_inv_get(const fe_save *s, const fe_unit *u, int slot, fe_inv_item *out)
{
    const uint8_t *p = s->data + u->off + UNIT_INV + slot * 5;
    out->id = p[1] | (p[2] << 8);
    out->uses = p[3];
    out->flags = p[4];
}

// Moves empty slots to the bottom, keeping the order of the others.
static void inv_pack(fe_save *s, const fe_unit *u)
{
    uint8_t tmp[FE_INV_SLOTS][5];
    int n = 0;
    for (int k = 0; k < FE_INV_SLOTS; k++) {
        uint8_t *p = inv_slot(s, u, k);
        if (p[1] | p[2])
            memcpy(tmp[n++], p, 5);
    }
    for (int k = 0; k < FE_INV_SLOTS; k++) {
        uint8_t *p = inv_slot(s, u, k);
        if (k < n) {
            memcpy(p, tmp[k], 5);
        } else {
            p[0] = 0x04;
            p[1] = p[2] = p[3] = p[4] = 0;
        }
    }
}

void fe_inv_set(fe_save *s, const fe_unit *u, int slot, int id, int uses)
{
    if (slot < 0 || slot >= FE_INV_SLOTS)
        return;
    uint8_t *p = inv_slot(s, u, slot);
    if (id <= 0) {
        p[1] = p[2] = p[3] = p[4] = 0;
    } else {
        bool same = (p[1] | (p[2] << 8)) == id;
        p[0] = 0x04;
        p[1] = (uint8_t)id;
        p[2] = (uint8_t)(id >> 8);
        p[3] = (uint8_t)(uses < 0 ? 0 : uses > 255 ? 255 : uses);
        if (!same)
            p[4] = 0;  // a new item is not equipped/dropped
    }
    inv_pack(s, u);
}

void fe_inv_equip(fe_save *s, const fe_unit *u, int slot)
{
    if (slot < 0 || slot >= FE_INV_SLOTS)
        return;
    uint8_t *p = inv_slot(s, u, slot);
    if (!(p[1] | p[2]))
        return;
    uint8_t chosen[5];
    memcpy(chosen, p, 5);
    chosen[4] |= FE_ITEM_EQUIPPED;
    // shift the slots above it down by one, put the equipped item on top
    for (int k = slot; k > 0; k--)
        memcpy(inv_slot(s, u, k), inv_slot(s, u, k - 1), 5);
    memcpy(inv_slot(s, u, 0), chosen, 5);
    for (int k = 1; k < FE_INV_SLOTS; k++)
        inv_slot(s, u, k)[4] &= (uint8_t)~FE_ITEM_EQUIPPED;
}

void fe_forge_name(const fe_save *s, int forge, char *out, size_t outlen)
{
    if (!outlen) return;
    out[0] = '\0';
    if (forge < 0 || forge >= s->forge_count) return;
    const uint8_t *r = s->data + s->refi_off + 7 + forge * FORGE_SIZE;
    size_t o = 0;
    for (int i = 2; i + 1 < 0x26 && o + 1 < outlen; i += 2) {
        uint16_t c = (uint16_t)(r[i] | (r[i + 1] << 8));
        if (!c) break;
        out[o++] = c < 0x80 ? (char)c : '?';
    }
    out[o] = '\0';
}

int fe_forge_base(const fe_save *s, int forge)
{
    if (forge < 0 || forge >= s->forge_count) return 0;
    const uint8_t *r = s->data + s->refi_off + 7 + forge * FORGE_SIZE;
    return r[0x26] | (r[0x27] << 8);
}

// Forge records are listed in order, but the slot number is stored in byte 0
static int forge_by_slot(const fe_save *s, int slot)
{
    for (int i = 0; i < s->forge_count; i++)
        if (s->data[s->refi_off + 7 + i * FORGE_SIZE] == slot)
            return i;
    return -1;
}

void fe_item_name(const fe_save *s, int id, char *out, size_t outlen)
{
    if (id >= 0 && id < forge_base_id(s)) {
        if (fe_item(id)) snprintf(out, outlen, "%s", fe_item(id)->name);
        else snprintf(out, outlen, "Item #%d", id);
        return;
    }
    int f = forge_by_slot(s, id - forge_base_id(s));
    if (f >= 0) {
        char nm[24];
        fe_forge_name(s, f, nm, sizeof(nm));
        snprintf(out, outlen, "%s*", nm);
    } else {
        snprintf(out, outlen, "Item #%d", id);
    }
}

int fe_item_max_uses(const fe_save *s, int id)
{
    if (id >= 0 && id < forge_base_id(s))
        return fe_item(id) ? fe_item(id)->uses : 0;
    int f = forge_by_slot(s, id - forge_base_id(s));
    int base = f >= 0 ? fe_forge_base(s, f) : 0;
    return base > 0 && fe_item(base) ? fe_item(base)->uses : 0;
}

int fe_convoy_get(const fe_save *s, int id)
{
    if (id < 0 || id >= s->convoy_count) return 0;
    const uint8_t *p = s->data + s->tran_off + 7 + id * 2;
    return p[0] | (p[1] << 8);
}

void fe_convoy_set(fe_save *s, int id, int total_uses)
{
    if (id <= 0 || id >= s->convoy_count) return;
    if (total_uses < 0) total_uses = 0;
    if (total_uses > 0xFFFF) total_uses = 0xFFFF;
    uint8_t *p = s->data + s->tran_off + 7 + id * 2;
    p[0] = (uint8_t)total_uses;
    p[1] = (uint8_t)(total_uses >> 8);
}

int fe_convoy_count_items(const fe_save *s, int id)
{
    int uses = fe_convoy_get(s, id), per = fe_item_max_uses(s, id);
    return per > 0 ? (uses + per - 1) / per : uses;
}

void fe_convoy_set_items(fe_save *s, int id, int count)
{
    int per = fe_item_max_uses(s, id);
    if (count < 0) count = 0;
    fe_convoy_set(s, id, per > 0 ? count * per : count);
}

//---------------------------------------------------------------------------
// Avatar appearance and name
//---------------------------------------------------------------------------
const uint8_t fe_hero_faces[FE_HERO_FACE_COUNT] = {
    11, 16, 24, 56, 67, 72, 80, 91, 96, 105, 114, 121, 128, 136, 144, 155};

#define HEADER_NAME_OFF 0x10

int fe_look_get(const fe_save *s, const fe_unit *u, int field)
{
    if (u->log_off < 0) return -1;
    return s->data[u->log_off + field];
}

void fe_look_set(fe_save *s, const fe_unit *u, int field, int value)
{
    if (u->log_off < 0) return;
    s->data[u->log_off + field] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

uint32_t fe_look_color(const fe_save *s, const fe_unit *u)
{
    if (u->log_off < 0) return 0;
    const uint8_t *c = s->data + u->log_off + FE_LOOK_COLOR;
    return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
}

// The unit's main record keeps its own hair color at AI block +0x39 (right after the
// learned-skill bits). Every Avatar in the test saves has it equal to the logbook color,
// and the 3D model uses this copy (hardware test 2026-10-02), so both are written.
#define UNIT_HAIR_COLOR (UNIT_FLAGS_SIZE + LEARNED_SIZE + 0x39)

void fe_look_set_color(fe_save *s, const fe_unit *u, uint32_t rgb)
{
    if (u->log_off < 0) return;
    uint8_t *copies[2] = {s->data + u->log_off + FE_LOOK_COLOR, s->data + u->flags_off + UNIT_HAIR_COLOR};
    for (int i = 0; i < 2; i++) {
        copies[i][0] = (uint8_t)(rgb >> 16);
        copies[i][1] = (uint8_t)(rgb >> 8);
        copies[i][2] = (uint8_t)rgb;
        copies[i][3] = 0xFF;
    }
}

// UTF-8 -> UTF-16 (BMP), at most max_chars, zero-padded to `bytes`.
static int utf8_to_utf16(const char *in, uint8_t *out, size_t bytes, int max_chars)
{
    memset(out, 0, bytes);
    int n = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && n < max_chars && (size_t)(n + 1) * 2 < bytes) {
        uint32_t c;
        if (*p < 0x80) c = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { c = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { c = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { p++; continue; }  // skip anything outside the BMP / malformed
        out[n * 2] = (uint8_t)c;
        out[n * 2 + 1] = (uint8_t)(c >> 8);
        n++;
    }
    return n;
}

int fe_unit_set_name(fe_save *s, const fe_unit *u, const char *utf8)
{
    if (u->log_off < 0) return -1;
    uint8_t *name = s->data + u->log_off;
    uint8_t old[LOG_NAME_SIZE];
    memcpy(old, name, LOG_NAME_SIZE);
    int n = utf8_to_utf16(utf8, name, LOG_NAME_SIZE, FE_NAME_MAX);

    // The slot header carries the Avatar's name for the file-select screen
    uint16_t id = fe_unit_char_id(s, u);
    bool main_avatar = (id == 0 || id == 1) && (u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE);
    if (main_avatar && memcmp(s->data + HEADER_NAME_OFF, old, LOG_NAME_SIZE) == 0)
        memcpy(s->data + HEADER_NAME_OFF, name, LOG_NAME_SIZE);
    return n;
}

const char *fe_hero_face_name(int face)
{
    for (int i = 0; i < FE_LOOK_PRESET_COUNT; i++)
        if (fe_look_presets[i].hero_head && fe_look_presets[i].face == face)
            return fe_look_presets[i].name;
    return NULL;
}

void fe_look_apply_preset(fe_save *s, const fe_unit *u, const fe_look_preset *p)
{
    if (u->log_off < 0) return;
    fe_look_set(s, u, FE_LOOK_GENDER, p->gender);
    fe_look_set(s, u, FE_LOOK_BUILD, p->build);
    fe_look_set(s, u, FE_LOOK_FACE, p->face);
    fe_look_set(s, u, FE_LOOK_HAIR, p->hair);
    fe_look_set(s, u, FE_LOOK_VOICE, p->voice);
    fe_look_set_color(s, u, p->color);
}

#define LOG_ID_OFF   0x27  // mainBlock 0x0D
#define LOG_HERO_OFF 0x37  // mainBlock 0x1D, bit 1

void fe_log_id_get(const fe_save *s, const fe_unit *u, uint8_t out[FE_LOG_ID_SIZE])
{
    memset(out, 0, FE_LOG_ID_SIZE);
    if (u->log_off >= 0) memcpy(out, s->data + u->log_off + LOG_ID_OFF, FE_LOG_ID_SIZE);
}

void fe_log_id_set(fe_save *s, const fe_unit *u, const uint8_t id[FE_LOG_ID_SIZE])
{
    if (u->log_off >= 0) memcpy(s->data + u->log_off + LOG_ID_OFF, id, FE_LOG_ID_SIZE);
}

bool fe_log_hero_flag(const fe_save *s, const fe_unit *u)
{
    return u->log_off >= 0 && (s->data[u->log_off + LOG_HERO_OFF] & 0x02);
}

void fe_log_set_hero_flag(fe_save *s, const fe_unit *u, bool on)
{
    if (u->log_off < 0) return;
    if (on) s->data[u->log_off + LOG_HERO_OFF] |= 0x02;
    else s->data[u->log_off + LOG_HERO_OFF] &= (uint8_t)~0x02;
}

const fe_look_preset *fe_log_hero_identity(const fe_save *s, const fe_unit *u)
{
    if (!fe_log_hero_flag(s, u)) return NULL;
    uint8_t id[FE_LOG_ID_SIZE];
    fe_log_id_get(s, u, id);
    for (int k = 1; k < FE_LOG_ID_SIZE; k++)
        if (id[k]) return NULL;
    for (int i = 0; i < FE_LOOK_PRESET_COUNT; i++)
        if (fe_look_presets[i].log_id == id[0]) return &fe_look_presets[i];
    return NULL;
}

void fe_log_set_hero_identity(fe_save *s, const fe_unit *u, const fe_look_preset *p)
{
    uint8_t id[FE_LOG_ID_SIZE] = {0};
    id[0] = p->log_id;
    fe_log_id_set(s, u, id);
    fe_log_set_hero_flag(s, u, true);
}

//---------------------------------------------------------------------------
// Adding units
//---------------------------------------------------------------------------
// Child trailer of a real Morgan (M) from a test save: father Avatar (M) asset 5 / flaw 1,
// mother Avatar (F), no grandparents. Parent ids/asset/flaw are overwritten.
static const uint8_t CHILD_TEMPLATE[UNIT_CHILD_SIZE] = {
    0x01, 0x01, 0x00, 0x00, 0x00, 0x05, 0x01, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00,
    0x01};
#define CHILD_FATHER 3   // u16 id, then asset, flaw
#define CHILD_MOTHER 20

static int log_byte(const fe_save *s, int ui, int field)
{
    return ui >= 0 && s->units[ui].log_off >= 0 ? s->data[s->units[ui].log_off + field] : 0;
}

void fe_unit_add_parents(const fe_save *s, int char_id, int *father, int *mother)
{
    *father = *mother = -1;
    const fe_char_recruit *rc = fe_char_recruit_of(char_id);
    if (!rc || rc->parent < 0)
        return;
    int fixed = rc->parent;
    int fi = fe_find_unit(s, fixed);
    int other = fi >= 0 ? fe_support_spouse(s, fi, -1) : -1;
    bool fixed_female = fe_char_recruit_of(fixed) && fe_char_recruit_of(fixed)->female;
    *mother = fixed_female ? fixed : other;
    *father = fixed_female ? other : fixed;
}

int fe_unit_add(fe_save *s, int template_index, int char_id, char *err, size_t errlen)
{
    if (!fe_char_addable(char_id) && char_id != 2) { fail(err, errlen, "unknown character %d", char_id); return -1; }
    if (template_index < 0 || template_index >= s->unit_count) { fail(err, errlen, "no template unit"); return -1; }

    // Where group 3 (army) ends, and its count byte
    uint32_t p = s->unit_off + 5, count_off = 0, pos = 0;
    int army_count = 0;
    for (int g = 0; g < FE_MAX_GROUPS; g++) {
        if (p + 2 > s->unit_end || s->data[p] != g) {
            if (g == FE_GROUP_ARMY) { fail(err, errlen, "this save has no army group"); return -1; }
            continue;
        }
        int n = s->data[p + 1];
        if (g == FE_GROUP_ARMY) { count_off = p + 1; army_count = n; }
        p += 2;
        for (int i = 0; i < s->unit_count; i++)
            if (s->units[i].group == g) p = s->units[i].off + s->units[i].size;
        if (g == FE_GROUP_ARMY) { pos = p; break; }
    }
    if (army_count >= 255) { fail(err, errlen, "the army already has 255 units"); return -1; }

    // Build the record from the template: fixed part with an empty support block
    const fe_unit *t = &s->units[template_index];
    const uint8_t *T = s->data + t->off;
    uint32_t sup = T[UNIT_SUPPORT_LEN_OFF];
    uint32_t unk_off = UNIT_SUPPORT_LEN_OFF + 1 + sup;
    uint32_t unk = T[unk_off];
    uint32_t fixed_old = UNIT_FIXED_SIZE + sup + unk;
    uint32_t fixed_new = UNIT_FIXED_SIZE + unk;
    static const fe_char_recruit unknown_rc = {0};
    const fe_char_recruit *rc = fe_char_recruit_of(char_id) ? fe_char_recruit_of(char_id) : &unknown_rc;
    bool child = rc->parent >= 0 && fe_char(char_id);
    uint32_t total = fixed_new + (child ? UNIT_CHILD_SIZE : 0);
    uint8_t *r = calloc(1, total);
    if (!r) { fail(err, errlen, "out of memory"); return -1; }
    memcpy(r, T, UNIT_SUPPORT_LEN_OFF);
    r[UNIT_SUPPORT_LEN_OFF] = 0;
    memcpy(r + UNIT_SUPPORT_LEN_OFF + 1, T + unk_off, fixed_old - unk_off);

    // Identity and a clean level-1 state (unknown ids keep the template's class)
    r[UNIT_CHAR_ID] = (uint8_t)char_id;
    r[UNIT_CHAR_ID + 1] = (uint8_t)(char_id >> 8);
    if (fe_char(char_id)) r[UNIT_CLASS] = rc->start_class;
    r[4] = r[5] = 0;
    static uint32_t salt = 0x9E3779B9u;  // per-unit id bytes 6..9: make them unique-ish
    salt = salt * 1664525u + 1013904223u + (uint32_t)char_id * 2654435761u + s->size;
    wr32(r + 6, salt);
    memset(r + UNIT_GAINS, 0, 8);
    r[UNIT_LEVEL] = 1;
    r[UNIT_EXP] = 0;
    r[0x15] = 0;  // boots
    for (int k = 0; k < 5; k++) {  // empty inventory
        uint8_t *it = r + 0x1A + k * 5;
        it[0] = 0x04; it[1] = it[2] = it[3] = it[4] = 0;
    }
    for (int k = 0; k < 5; k++) {  // personal skills equipped
        r[UNIT_SKILLS + 2 * k] = rc->skills[k];
        r[UNIT_SKILLS + 2 * k + 1] = 0;
    }
    uint8_t *flags = r + UNIT_SUPPORT_LEN_OFF + 1 + 1 + unk;
    flags[2] = 0xFF;  // no deployment slot yet
    uint8_t *learned = flags + UNIT_FLAGS_SIZE;
    memset(learned, 0, LEARNED_SIZE);
    for (int k = 0; k < 5; k++)
        if (rc->skills[k]) learned[rc->skills[k] / 8] |= (uint8_t)(1 << (rc->skills[k] % 8));
    // Trailer marker: the last two bytes of the fixed part
    r[fixed_new - 2] = 0x00;
    r[fixed_new - 1] = child ? 0x01 : 0x00;

    if (child) {
        uint8_t *c = r + fixed_new;
        memcpy(c, CHILD_TEMPLATE, UNIT_CHILD_SIZE);
        int father, mother;
        fe_unit_add_parents(s, char_id, &father, &mother);
        int pf = father >= 0 ? fe_find_unit(s, father) : -1, pm = mother >= 0 ? fe_find_unit(s, mother) : -1;
        uint16_t fid = father >= 0 ? (uint16_t)father : 0xFFFF, mid = mother >= 0 ? (uint16_t)mother : 0xFFFF;
        c[CHILD_FATHER] = (uint8_t)fid; c[CHILD_FATHER + 1] = (uint8_t)(fid >> 8);
        c[CHILD_FATHER + 2] = (uint8_t)log_byte(s, pf, 0x1A); c[CHILD_FATHER + 3] = (uint8_t)log_byte(s, pf, 0x1B);
        c[CHILD_MOTHER] = (uint8_t)mid; c[CHILD_MOTHER + 1] = (uint8_t)(mid >> 8);
        c[CHILD_MOTHER + 2] = (uint8_t)log_byte(s, pm, 0x1A); c[CHILD_MOTHER + 3] = (uint8_t)log_byte(s, pm, 0x1B);
    }

    uint8_t newcount = (uint8_t)(army_count + 1);
    bool ok = splice_save(s, pos, r, total, &count_off, &newcount, 1, s->unit_count + 1, err, errlen);
    free(r);
    if (!ok) return -1;

    // Find the new unit (last of the army group) and give it full HP
    int ni = -1;
    for (int i = 0; i < s->unit_count; i++)
        if (s->units[i].group == FE_GROUP_ARMY && s->units[i].off == pos) ni = i;
    if (ni < 0) { fail(err, errlen, "new unit not found after insert"); return -1; }
    fe_stats st;
    if (fe_unit_stats(s, &s->units[ni], &st)) {
        int hp = fe_stat_shown(&st, 0);
        s->data[s->units[ni].off + UNIT_CUR_HP] = (uint8_t)(hp > 255 ? 255 : hp);
    }
    return ni;
}

bool fe_char_addable(int char_id)
{
    return fe_char(char_id) && fe_char_vanilla_id(char_id) != 2;
}

bool fe_unit_add_logbook(fe_save *s, int unit_index, int src_index, char *err, size_t errlen)
{
    if (unit_index < 0 || unit_index >= s->unit_count || src_index < 0 || src_index >= s->unit_count)
        return fail(err, errlen, "invalid unit");
    const fe_unit *u = &s->units[unit_index];
    const fe_unit *src = &s->units[src_index];
    if (u->log_off >= 0) return true;  // already has one
    if (u->child_off >= 0) return fail(err, errlen, "child units cannot get Avatar logbook data");
    if (src->log_off < 0) return fail(err, errlen, "no Avatar logbook data to copy from");

    uint8_t log[UNIT_LOG_SIZE_US];
    memcpy(log, s->data + src->log_off, UNIT_LOG_SIZE_US);
    uint16_t id = fe_unit_char_id(s, u);
    bool female = fe_char_recruit_of(id) && fe_char_recruit_of(id)->female;

    // name "Robin"
    memset(log, 0, LOG_NAME_SIZE);
    const char *nm = "Robin";
    for (int i = 0; nm[i]; i++) log[i * 2] = (uint8_t)nm[i];
    log[FE_LOOK_GENDER] = female ? 1 : 0;
    log[FE_LOOK_BUILD] = 1;
    log[FE_LOOK_FACE] = 0;
    log[FE_LOOK_HAIR] = 0;
    log[FE_LOOK_VOICE] = 0;
    log[FE_LOOK_COLOR + 0] = 0xF6; log[FE_LOOK_COLOR + 1] = 0xF4; log[FE_LOOK_COLOR + 2] = 0xEF; log[FE_LOOK_COLOR + 3] = 0xFF;
    // own random logbook id, not a hero
    static uint32_t seed = 0x2545F491u;
    for (int k = 0; k < FE_LOG_ID_SIZE; k++) {
        seed = seed * 1103515245u + 12345u + s->size + (uint32_t)k;
        log[0x27 + k] = (uint8_t)(seed >> 16);
    }
    log[0x37] &= (uint8_t)~0x02;
    log[UNIT_LOG_SIZE_US - 1] = 0;  // no child block after it

    uint32_t fixed_end = u->off + u->size;            // no trailers, so the record ends here
    uint32_t marker[2] = {fixed_end - 2, fixed_end - 1};
    uint8_t vals[2] = {0x01, 0x06};                   // "a logbook block follows"
    if (!splice_save(s, fixed_end, log, UNIT_LOG_SIZE_US, marker, vals, 2, s->unit_count, err, errlen))
        return false;
    fe_look_set_color(s, &s->units[unit_index], 0xF6F4EF);  // also the main-record copy
    return true;
}

//---------------------------------------------------------------------------
// Weapon ranks, Boots, battle records
//---------------------------------------------------------------------------
#define UNIT_WEXP      (UNIT_SKILLS + 10)  // 6 bytes: sword, lance, axe, bow, tome, staff
#define UNIT_BOOTS     0x15
#define FLAGS_BATTLE   0x0F                // bit 3 fallen (Classic), bit 7 fell on this map
#define END_BATTLES    0x31                // u16 battles, then u16 victories

const char *const fe_weapon_names[FE_WEAPON_COUNT] = {"Sword", "Lance", "Axe", "Bow", "Tome", "Staff"};
static const int WEXP_RANK_AT[5] = {0, 15, 35, 60, 90};  // E D C B A

static uint32_t end_off(const fe_unit *u) { return u->flags_off + UNIT_FLAGS_SIZE + LEARNED_SIZE; }

int fe_unit_wexp(const fe_save *s, const fe_unit *u, int w)
{
    return s->data[u->off + UNIT_WEXP + w];
}

void fe_unit_set_wexp(fe_save *s, const fe_unit *u, int w, int v)
{
    s->data[u->off + UNIT_WEXP + w] = (uint8_t)(v < 0 ? 0 : v > FE_WEXP_MAX ? FE_WEXP_MAX : v);
}

const char *fe_wexp_rank(int v)
{
    static const char *const names[5] = {"E", "D", "C", "B", "A"};
    int r = 0;
    while (r < 4 && v >= WEXP_RANK_AT[r + 1]) r++;
    return names[r];
}

int fe_wexp_next_rank(int v)
{
    for (int r = 0; r < 5; r++)
        if (WEXP_RANK_AT[r] > v) return WEXP_RANK_AT[r];
    return FE_WEXP_MAX;
}

int fe_wexp_prev_rank(int v)
{
    for (int r = 4; r >= 0; r--)
        if (WEXP_RANK_AT[r] < v) return WEXP_RANK_AT[r];
    return 0;
}

int fe_unit_boots(const fe_save *s, const fe_unit *u) { return s->data[u->off + UNIT_BOOTS]; }

void fe_unit_set_boots(fe_save *s, const fe_unit *u, int v)
{
    s->data[u->off + UNIT_BOOTS] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int fe_unit_record(const fe_save *s, const fe_unit *u, bool victories)
{
    const uint8_t *p = s->data + end_off(u) + END_BATTLES + (victories ? 2 : 0);
    return p[0] | (p[1] << 8);
}

void fe_unit_set_record(fe_save *s, const fe_unit *u, bool victories, int v)
{
    uint8_t *p = s->data + end_off(u) + END_BATTLES + (victories ? 2 : 0);
    v = v < 0 ? 0 : v > 65535 ? 65535 : v;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

bool fe_unit_heal(fe_save *s, const fe_unit *u)
{
    fe_stats st;
    if (!fe_unit_stats(s, u, &st)) return false;
    int hp = fe_stat_shown(&st, 0);
    uint8_t v = (uint8_t)(hp > 255 ? 255 : hp);
    if (s->data[u->off + UNIT_CUR_HP] == v) return false;
    s->data[u->off + UNIT_CUR_HP] = v;
    return true;
}

//---------------------------------------------------------------------------
// Moving and removing units
//---------------------------------------------------------------------------
// Rewrites the UNIT block with every unit in group new_group[i] (-1 = removed). Units keep
// their order; units moved into a group go after the ones already there.
static bool regroup_units(fe_save *s, const int *new_group, char *err, size_t errlen)
{
    uint32_t body = s->unit_off + 5, tail = body;
    size_t total = 0;
    int kept = 0;
    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        if (u->off + u->size > tail) tail = u->off + u->size;
        if (new_group[i] >= 0) { total += u->size; kept++; }
    }
    uint8_t *buf = malloc(total + 2 * FE_MAX_GROUPS);
    if (!buf) return fail(err, errlen, "out of memory");
    size_t o = 0;
    for (int g = 0; g < FE_MAX_GROUPS; g++) {
        int n = 0;
        for (int i = 0; i < s->unit_count; i++) n += new_group[i] == g;
        if (!n) continue;
        if (n > 255) { free(buf); return fail(err, errlen, "a unit group would hold more than 255 units"); }
        buf[o++] = (uint8_t)g;
        buf[o++] = (uint8_t)n;
        for (int pass = 0; pass < 2; pass++)  // units already in g, then the ones moving in
            for (int i = 0; i < s->unit_count; i++) {
                const fe_unit *u = &s->units[i];
                if (new_group[i] == g && ((u->group == g) == (pass == 0))) {
                    memcpy(buf + o, s->data + u->off, u->size);
                    o += u->size;
                }
            }
    }
    bool ok = splice_replace(s, body, tail - body, buf, (uint32_t)o, NULL, NULL, 0, kept, err, errlen);
    free(buf);
    return ok;
}

bool fe_unit_revive(fe_save *s, int unit_index, char *err, size_t errlen)
{
    if (unit_index < 0 || unit_index >= s->unit_count) return fail(err, errlen, "invalid unit");
    const fe_unit *u = &s->units[unit_index];
    if (u->group != FE_GROUP_DEAD) return fail(err, errlen, "this unit has not fallen");
    int *ng = malloc(sizeof(int) * (size_t)s->unit_count);
    if (!ng) return fail(err, errlen, "out of memory");
    for (int i = 0; i < s->unit_count; i++) ng[i] = s->units[i].group;
    ng[unit_index] = FE_GROUP_ARMY;
    uint8_t saved_flag = s->data[u->flags_off + FLAGS_BATTLE];
    uint8_t saved_hp = s->data[u->off + UNIT_CUR_HP];
    s->data[u->flags_off + FLAGS_BATTLE] &= (uint8_t)~0x88;
    fe_unit_heal(s, u);
    bool ok = regroup_units(s, ng, err, errlen);
    free(ng);
    if (!ok) {  // put the record back as it was
        s->data[s->units[unit_index].flags_off + FLAGS_BATTLE] = saved_flag;
        s->data[s->units[unit_index].off + UNIT_CUR_HP] = saved_hp;
    }
    return ok;
}

bool fe_unit_remove(fe_save *s, int unit_index, char *err, size_t errlen)
{
    if (unit_index < 0 || unit_index >= s->unit_count) return fail(err, errlen, "invalid unit");
    int *ng = malloc(sizeof(int) * (size_t)s->unit_count);
    if (!ng) return fail(err, errlen, "out of memory");
    for (int i = 0; i < s->unit_count; i++) ng[i] = s->units[i].group;
    ng[unit_index] = -1;
    bool ok = regroup_units(s, ng, err, errlen);
    free(ng);
    return ok;
}

//---------------------------------------------------------------------------
// World map and story progress
//---------------------------------------------------------------------------
#define GMAP_HDR    0x3E
#define GMAP_ENTRY  0x1D
#define USER_STORY  0x0F

// Index = map id (FireEditor chapters.xml)
static const char *const MAP_NAMES[51][2] = {
    {"Prologue", "Southtown"}, {"Chapter 1", "West of Ylisstol"}, {"Chapter 2", "The Northroad"},
    {"Chapter 3", "The Longfort"}, {"Chapter 4", "Arena Ferox"}, {"Chapter 5", "Border Pass"},
    {"Chapter 6", "Ylisstol"}, {"Chapter 7", "Breakneck Pass"}, {"Chapter 8", "Border Sands"},
    {"Chapter 9", "Plegia Castle Courtyard"}, {"Chapter 10", "The Midmire"}, {"Chapter 11", "Border Wastes"},
    {"Chapter 12", "Port Ferox"}, {"Chapter 13", "Carrion Isle"}, {"Chapter 14", "The Searoad"},
    {"Chapter 15", "Valm Harbor"}, {"Chapter 16", "The Mila Tree"}, {"Chapter 17", "Fort Steiger"},
    {"Chapter 18", "The Demon's Ingle"}, {"Chapter 19", "Valm Castle Approach"}, {"Chapter 20", "Valm Castle"},
    {"Chapter 21", "Plegia Castle"}, {"Chapter 22", "Table Approach"}, {"Chapter 23", "The Dragon's Table"},
    {"Chapter 24", "Mount Prism"}, {"Chapter 25", "Origin Peak"}, {"Endgame", "Grima"},
    {"Paralogue 1", "The Farfort"}, {"Paralogue 2", "The Twins' Turf"}, {"Paralogue 3", "Peaceful Village"},
    {"Paralogue 4", "The Twins' Hideout"}, {"Paralogue 5", "Sage's Hamlet"}, {"Paralogue 6", "Great Gate"},
    {"Paralogue 7", "Mila Shrine Ruins"}, {"Paralogue 8", "Dueling Grounds"}, {"Paralogue 9", "Verdant Forest"},
    {"Paralogue 10", "Mercenary Fortress"}, {"Paralogue 11", "Wyvern Valley"}, {"Paralogue 12", "The Ruins of Time"},
    {"Paralogue 13", "Law's End"}, {"Paralogue 14", "Desert Oasis"}, {"Paralogue 15", "Kidnapper's Keep"},
    {"Paralogue 16", "Manor of Lost Souls"}, {"Paralogue 17", "Divine Dragon Grounds"},
    {"Paralogue 18", "Sea-King's Throne"}, {"Paralogue 19", "Conqueror's Whetstone"},
    {"Paralogue 20", "Mountain Village"}, {"Paralogue 21", "Warriors' Tomb"}, {"Paralogue 22", "Wellspring of Truth"},
    {"Paralogue 23", "Garden of Giants"}, {"Outrealm Gate", "Outrealm Gate"}};

int fe_map_state(const fe_save *s, int loc)
{
    if (!s->gmap_off || loc < 0 || loc >= s->map_count) return -1;
    return s->data[s->gmap_off + GMAP_HDR + (uint32_t)loc * GMAP_ENTRY + 1];
}

void fe_map_set_state(fe_save *s, int loc, int state)
{
    if (!s->gmap_off || loc < 0 || loc >= s->map_count || state < 0 || state > 2) return;
    s->data[s->gmap_off + GMAP_HDR + (uint32_t)loc * GMAP_ENTRY + 1] = (uint8_t)state;
}

const char *fe_map_name(int loc)
{
    static char buf[16];
    if (loc >= 0 && loc < 51) return MAP_NAMES[loc][0];
    snprintf(buf, sizeof(buf), "Map #%d", loc);
    return buf;
}

const char *fe_map_place(int loc)
{
    return loc >= 0 && loc < 51 ? MAP_NAMES[loc][1] : "";
}

int fe_story_chapter(const fe_save *s) { return s->data[s->user_off + USER_STORY]; }

void fe_story_set_chapter(fe_save *s, int id)
{
    if (id >= 0 && id < 256) s->data[s->user_off + USER_STORY] = (uint8_t)id;
}

const char *fe_chapter_name(int id)
{
    static char buf[24];
    if (id == 1) return "Premonition";
    if (id >= 2 && id < 2 + 51) return MAP_NAMES[id - 2][0];
    snprintf(buf, sizeof(buf), "Chapter id %d", id);
    return buf;
}

int fe_unit_insert(fe_save *s, const uint8_t *rec, uint32_t len, char *err, size_t errlen)
{
    uint32_t p = s->unit_off + 5, count_off = 0, pos = 0;
    int army_count = 0;
    for (int g = 0; g < FE_MAX_GROUPS; g++) {
        if (p + 2 > s->unit_end || s->data[p] != g) {
            if (g == FE_GROUP_ARMY) { fail(err, errlen, "this save has no army group"); return -1; }
            continue;
        }
        int n = s->data[p + 1];
        if (g == FE_GROUP_ARMY) { count_off = p + 1; army_count = n; }
        p += 2;
        for (int i = 0; i < s->unit_count; i++)
            if (s->units[i].group == g) p = s->units[i].off + s->units[i].size;
        if (g == FE_GROUP_ARMY) { pos = p; break; }
    }
    if (army_count >= 255) { fail(err, errlen, "the army already has 255 units"); return -1; }
    uint8_t newcount = (uint8_t)(army_count + 1);
    if (!splice_save(s, pos, rec, len, &count_off, &newcount, 1, s->unit_count + 1, err, errlen))
        return -1;
    for (int i = 0; i < s->unit_count; i++)
        if (s->units[i].group == FE_GROUP_ARMY && s->units[i].off == pos) {
            if (s->units[i].size != len) { fail(err, errlen, "inserted unit parsed with the wrong size"); return -1; }
            return i;
        }
    fail(err, errlen, "new unit not found after insert");
    return -1;
}

int fe_item_base(const fe_save *s, int id)
{
    if (id >= 0 && id < forge_base_id(s)) return id;
    int f = forge_by_slot(s, id - forge_base_id(s));
    return f >= 0 ? fe_forge_base(s, f) : 0;
}

bool fe_save_clone(const fe_save *src, fe_save *dst)
{
    *dst = *src;
    dst->data = malloc(src->size);
    if (!dst->data) return false;
    memcpy(dst->data, src->data, src->size);
    return true;
}

int fe_support_a_rank(int type)
{
    int v = 0;
    for (int k = 0; k < 3; k++) {  // C, B, A ready
        int n = fe_support_next_ready(type, v);
        if (n < 0) return v + 1;
        v = n;
    }
    return v + 1;
}

bool fe_support_set_own(fe_save *s, int unit_index, int partner_char, int value, char *err, size_t errlen)
{
    if (unit_index < 0 || unit_index >= s->unit_count)
        return fail(err, errlen, "invalid unit");
    return set_one_side(s, unit_index, partner_char, value < 0 ? 0 : value > 255 ? 255 : value, err, errlen);
}

//---------------------------------------------------------------------------
// Difficulty and mode
//---------------------------------------------------------------------------
#define HDR_MODE  0x08
#define HDR_LPLUS 0x09
#define HDR_DIFF  0x0D

int fe_difficulty(const fe_save *s) { return s->data[s->gold_off - 1]; }

void fe_set_difficulty(fe_save *s, int d)
{
    if (d < 0 || d > 2) return;
    s->data[s->gold_off - 1] = (uint8_t)d;
    s->data[HDR_DIFF] = (uint8_t)d;
}

bool fe_is_casual(const fe_save *s) { return (s->data[s->gold_off - 6] & 0x04) != 0; }

void fe_set_casual(fe_save *s, bool on)
{
    uint8_t *a = &s->data[s->gold_off - 6], *b = &s->data[HDR_MODE];
    if (on) { *a |= 0x04; *b |= 0x04; }
    else { *a &= (uint8_t)~0x04; *b &= (uint8_t)~0x04; }
}

bool fe_is_lunatic_plus(const fe_save *s) { return (s->data[s->gold_off - 5] & 0x01) != 0; }

void fe_set_lunatic_plus(fe_save *s, bool on)
{
    uint8_t *a = &s->data[s->gold_off - 5], *b = &s->data[HDR_LPLUS];
    if (on) { *a |= 0x01; *b |= 0x01; }
    else { *a &= (uint8_t)~0x01; *b &= (uint8_t)~0x01; }
}

//---------------------------------------------------------------------------
// Parents
//---------------------------------------------------------------------------
#define CHILD_SIDE 0x11

static uint8_t *parent_entry(const fe_save *s, const fe_unit *u, int side, int slot)
{
    return s->data + u->child_off + CHILD_FATHER + side * CHILD_SIDE + slot * 5;
}

int fe_child_parent(const fe_save *s, const fe_unit *u, int side)
{
    if (u->child_off < 0) return -1;
    const uint8_t *e = parent_entry(s, u, side, 0);
    int id = e[0] | e[1] << 8;
    return id == 0xFFFF ? -1 : id;
}

void fe_child_set_parent(fe_save *s, const fe_unit *u, int side, int char_id)
{
    if (u->child_off < 0 || side < 0 || side > 1) return;
    int pu = char_id >= 0 ? fe_find_unit(s, char_id) : -1;
    uint8_t *e = parent_entry(s, u, side, 0);
    uint16_t id = char_id >= 0 ? (uint16_t)char_id : 0xFFFF;
    e[0] = (uint8_t)id; e[1] = (uint8_t)(id >> 8);
    e[2] = (uint8_t)log_byte(s, pu, 0x1A);
    e[3] = (uint8_t)log_byte(s, pu, 0x1B);
    for (int g = 1; g <= 2; g++) {  // grandparents: the parent's own parents
        uint8_t *ge = parent_entry(s, u, side, g);
        if (pu >= 0 && s->units[pu].child_off >= 0) {
            memcpy(ge, parent_entry(s, &s->units[pu], g - 1, 0), 4);
        } else {
            ge[0] = ge[1] = 0xFF;
            ge[2] = ge[3] = 0;
        }
    }
}

uint32_t fe_unit_hair(const fe_save *s, const fe_unit *u)
{
    const uint8_t *p = s->data + u->flags_off + UNIT_HAIR_COLOR;
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

void fe_unit_set_hair(fe_save *s, const fe_unit *u, uint32_t rgb)
{
    if (u->log_off >= 0) { fe_look_set_color(s, u, rgb); return; }
    uint8_t *p = s->data + u->flags_off + UNIT_HAIR_COLOR;
    p[0] = (uint8_t)(rgb >> 16);
    p[1] = (uint8_t)(rgb >> 8);
    p[2] = (uint8_t)rgb;
    p[3] = 0xFF;
}

uint32_t fe_child_default_hair(const fe_save *s, const fe_unit *u)
{
    int c = fe_unit_char_id(s, u);
    const fe_char_recruit *rc = fe_char_recruit_of(c);
    if (!rc) return fe_unit_hair(s, u);
    if (!rc->custom_hair || u->child_off < 0) return rc->hair;
    int f = fe_child_parent(s, u, 0), m = fe_child_parent(s, u, 1);
    int other = f == rc->parent ? m : m == rc->parent ? f : (f >= 0 ? f : m);
    if (other < 0) return rc->hair;
    int ou = fe_find_unit(s, other);
    if (ou >= 0) return fe_unit_hair(s, &s->units[ou]);
    return fe_char_recruit_of(other) ? fe_char_recruit_of(other)->hair : rc->hair;
}

int fe_child_inherit_skills(fe_save *s, const fe_unit *u)
{
    int n = 0;
    for (int side = 0; side < 2; side++) {
        int p = fe_child_parent(s, u, side);
        int pu = p >= 0 ? fe_find_unit(s, p) : -1;
        if (pu < 0) continue;
        for (int k = FE_SKILL_FIRST; k <= FE_SKILL_LAST; k++)
            if (fe_unit_is_learned(s, &s->units[pu], k) && !fe_unit_is_learned(s, u, k)) {
                fe_unit_set_learned(s, u, k, true);
                n++;
            }
    }
    return n;
}

//---------------------------------------------------------------------------
// Forge editing
//---------------------------------------------------------------------------
int fe_forge_of_item(const fe_save *s, int id)
{
    return id >= forge_base_id(s) ? forge_by_slot(s, id - forge_base_id(s)) : -1;
}

void fe_forge_bonus(const fe_save *s, int f, int *mt, int *hit, int *crit)
{
    *mt = *hit = *crit = 0;
    if (f < 0 || f >= s->forge_count) return;
    const uint8_t *r = s->data + s->refi_off + 7 + f * FORGE_SIZE;
    *mt = r[0x28]; *hit = r[0x29]; *crit = r[0x2A];
}

static uint8_t clamp_bonus(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void fe_forge_edit(fe_save *s, int f, const char *name, int mt, int hit, int crit)
{
    if (f < 0 || f >= s->forge_count) return;
    uint8_t *r = s->data + s->refi_off + 7 + f * FORGE_SIZE;
    if (name) utf8_to_utf16(name, r + 2, 0x24, FE_FORGE_NAME_MAX);
    r[0x28] = clamp_bonus(mt); r[0x29] = clamp_bonus(hit); r[0x2A] = clamp_bonus(crit);
}

int fe_forge_create(fe_save *s, int base, const char *name, int mt, int hit, int crit, char *err, size_t errlen)
{
    if (base <= 0 || base >= forge_base_id(s) || !fe_item(base)) { fail(err, errlen, "not a regular item"); return -1; }
    if (s->forge_count >= FE_FORGE_MAX) { fail(err, errlen, "all %d forge slots are used", FE_FORGE_MAX); return -1; }
    if (s->convoy_count < forge_base_id(s) + FE_FORGE_MAX) { fail(err, errlen, "this save has no room for forges"); return -1; }
    int slot = -1;
    for (int k = 0; k < FE_FORGE_MAX && slot < 0; k++)
        if (forge_by_slot(s, k) < 0) slot = k;
    if (slot < 0) { fail(err, errlen, "no free forge slot"); return -1; }
    uint8_t r[FORGE_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = (uint8_t)slot;
    utf8_to_utf16(name && name[0] ? name : fe_item(base)->name, r + 2, 0x24, FE_FORGE_NAME_MAX);
    r[0x26] = (uint8_t)base; r[0x27] = (uint8_t)(base >> 8);
    r[0x28] = clamp_bonus(mt); r[0x29] = clamp_bonus(hit); r[0x2A] = clamp_bonus(crit);
    uint32_t count_off = s->refi_off + 5;
    uint8_t newcount = (uint8_t)(s->forge_count + 1);
    uint32_t pos = s->refi_off + 7 + (uint32_t)s->forge_count * FORGE_SIZE;
    if (!splice_save(s, pos, r, FORGE_SIZE, &count_off, &newcount, 1, s->unit_count, err, errlen)) return -1;
    return forge_base_id(s) + slot;
}

static uint8_t *enc_slot(const fe_save *s, int loc, int k)
{
    return s->data + s->gmap_off + GMAP_HDR + (uint32_t)loc * GMAP_ENTRY + 3 + (uint32_t)k * 0x0D;
}

static int enc_type(const uint8_t *e)
{
    if (e[0] == 1 && e[1] == 1) return FE_ENC_RISEN;
    if (e[0] == 3 && e[1] == 2) return FE_ENC_MERCHANT;
    if (e[0] == 2 && e[1] == 2) return FE_ENC_TEAM;
    return FE_ENC_NONE;
}

int fe_map_encounter(const fe_save *s, int loc)
{
    if (!s->gmap_off || loc < 0 || loc >= s->map_count) return FE_ENC_NONE;
    int t0 = enc_type(enc_slot(s, loc, 0)), t1 = enc_type(enc_slot(s, loc, 1));
    if (t0 == FE_ENC_RISEN || t0 == FE_ENC_MERCHANT) return t0;
    if (t1 == FE_ENC_RISEN || t1 == FE_ENC_MERCHANT) return t1;
    return t0 == FE_ENC_TEAM || t1 == FE_ENC_TEAM ? FE_ENC_TEAM : FE_ENC_NONE;
}

static void enc_write(uint8_t *e, int type, uint32_t rnd)
{
    static const uint8_t RISEN_CLASSES[] = {21, 25, 26, 44, 54};  // map classes seen in real saves
    if (type == FE_ENC_NONE) {
        e[0] = e[1] = 0;
        e[2] = e[3] = 0xFF;
        e[4] = e[5] = 0;
        e[6] = 0;
        e[7] = 0xFF;
        memset(e + 8, 0, 4);
        return;
    }
    e[0] = type == FE_ENC_RISEN ? 1 : 3;
    e[1] = type == FE_ENC_RISEN ? 1 : 2;
    int cls = type == FE_ENC_RISEN ? RISEN_CLASSES[rnd % sizeof(RISEN_CLASSES)] : 73;
    e[2] = (uint8_t)cls; e[3] = 0;
    e[4] = (uint8_t)(16 + (rnd >> 8) % 8); e[5] = 0;  // pool, like the game's mid-story values
    e[6] = 16;                                          // days left
    e[7] = 0xFF;
    uint32_t seed = rnd * 2654435761u + 0x9E3779B9u;
    e[8] = (uint8_t)seed; e[9] = (uint8_t)(seed >> 8); e[10] = (uint8_t)(seed >> 16); e[11] = (uint8_t)(seed >> 24);
}

void fe_map_set_encounter(fe_save *s, int loc, int type, uint32_t rnd)
{
    if (!s->gmap_off || loc < 0 || loc >= s->map_count) return;
    uint8_t *e0 = enc_slot(s, loc, 0), *e1 = enc_slot(s, loc, 1);
    bool team0 = enc_type(e0) == FE_ENC_TEAM, team1 = enc_type(e1) == FE_ENC_TEAM;
    if (!team1) enc_write(e1, FE_ENC_NONE, 0);
    if (type == FE_ENC_NONE || type == FE_ENC_TEAM) {
        if (!team0) enc_write(e0, FE_ENC_NONE, 0);
        return;
    }
    if (!team0) enc_write(e0, type, rnd);
    else if (!team1) enc_write(e1, type, rnd);
}

//---------------------------------------------------------------------------
// Play time and chapter records
//---------------------------------------------------------------------------
uint32_t fe_playtime(const fe_save *s) { return rd32(s->data + s->user_off + 5); }

void fe_set_playtime(fe_save *s, uint32_t frames)
{
    wr32(s->data + s->user_off + 5, frames);
    wr32(s->data + 1, frames);  // file header copy (shown on the load screen)
}

int fe_record_count(const fe_save *s) { return s->data[s->user_off + 0x15]; }

static uint8_t *record_ptr(const fe_save *s, int k) { return s->data + s->user_off + 0x16 + (uint32_t)k * 0x10; }

static int rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void wr16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void fe_record_get(const fe_save *s, int k, fe_record *out)
{
    memset(out, 0, sizeof(*out));
    if (k < 0 || k >= fe_record_count(s)) return;
    const uint8_t *r = record_ptr(s, k);
    out->chapter = r[1];
    out->turns = rd16(r + 2);
    out->frames = rd32(r + 4);
    for (int i = 0; i < 2; i++) {
        int u = rd16(r + 8 + i * 4), c = rd16(r + 10 + i * 4);
        out->unit[i] = u == 0xFFFF ? -1 : u;
        out->cls[i] = c == 0xFFFF ? -1 : c;
    }
}

void fe_record_set(fe_save *s, int k, const fe_record *in)
{
    if (k < 0 || k >= fe_record_count(s)) return;
    uint8_t *r = record_ptr(s, k);
    r[1] = (uint8_t)in->chapter;
    wr16(r + 2, in->turns < 0 ? 0 : in->turns > 0xFFFF ? 0xFFFF : in->turns);
    wr32(r + 4, in->frames);
    for (int i = 0; i < 2; i++) {
        wr16(r + 8 + i * 4, in->unit[i] < 0 ? 0xFFFF : in->unit[i]);
        wr16(r + 10 + i * 4, in->cls[i] < 0 ? 0xFFFF : in->cls[i]);
    }
}

//---------------------------------------------------------------------------
// Renown rewards and Barracks events
//---------------------------------------------------------------------------
#define RENOWN_CLAIMS 0x12  // gold + 0x12 = USER tail + 0x7B, 5 bytes

int fe_renown_claimed(const fe_save *s)
{
    int n = 0;
    for (int i = 0; i < 5; i++)
        for (int b = 0; b < 8; b++) n += (s->data[s->gold_off + RENOWN_CLAIMS + i] >> b) & 1;
    return n;
}

void fe_renown_reset_claims(fe_save *s)
{
    memset(s->data + s->gold_off + RENOWN_CLAIMS, 0, 5);
}

const char *const fe_barracks_names[FE_BEV_TYPES] = {"(none)", "Stat boost", "Exp", "Weapon exp",
                                                     "Found item", "Conversation", "Birthday"};

static uint32_t evst_off(const fe_save *s)
{
    int b = s->is_map ? 1 : 0;
    uint32_t ev = rd32(s->data + s->hdr_size + 4 + 4 * (uint32_t)(b + 6));
    if (!ev || ev + 0x50 + 8 * FE_BARRACKS_SLOTS > s->size || memcmp(s->data + ev, "TSVE", 4) != 0) return 0;
    return ev;
}

bool fe_barracks_ok(const fe_save *s) { return evst_off(s) != 0; }

void fe_barracks_get(const fe_save *s, int k, fe_barracks_event *out)
{
    memset(out, 0, sizeof(*out));
    out->unit[0] = out->unit[1] = -1;
    uint32_t ev = evst_off(s);
    if (!ev || k < 0 || k >= FE_BARRACKS_SLOTS) return;
    const uint8_t *e = s->data + ev + 0x50 + k * 8;
    out->type = e[2] < FE_BEV_TYPES ? e[2] : 0;
    for (int i = 0; i < 2; i++) {
        int u = e[3 + 2 * i] | e[4 + 2 * i] << 8;
        out->unit[i] = u == 0xFFFF ? -1 : u;
    }
    out->icon = e[7];
}

void fe_barracks_set(fe_save *s, int k, const fe_barracks_event *in)
{
    uint32_t ev = evst_off(s);
    if (!ev || k < 0 || k >= FE_BARRACKS_SLOTS) return;
    uint8_t *e = s->data + ev + 0x50 + k * 8;
    int u[2] = {in->unit[0], in->unit[1]};
    if (u[0] < 0 && u[1] >= 0) { u[0] = u[1]; u[1] = -1; }  // the first unit must be filled first
    if (in->type == FE_BEV_NONE) { u[0] = u[1] = -1; }
    if (in->type != FE_BEV_TALK) u[1] = -1;  // only conversations use a second unit (hardware test)
    e[0] = 0;
    e[1] = (uint8_t)((u[0] >= 0) + (u[1] >= 0));
    e[2] = (uint8_t)in->type;
    for (int i = 0; i < 2; i++) {
        uint16_t v = u[i] < 0 ? 0xFFFF : (uint16_t)u[i];
        e[3 + 2 * i] = (uint8_t)v;
        e[4 + 2 * i] = (uint8_t)(v >> 8);
    }
    e[7] = in->type == FE_BEV_NONE ? 0xFF : (uint8_t)in->icon;
}
