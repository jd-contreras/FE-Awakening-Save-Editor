#include "core/feimport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fedata.h"

// Unit record layout (see SAVE_FORMAT.md)
#define R_CHAR      0x01
#define R_CLASS     0x03
#define R_UNIQUE    0x06  // 4 bytes, unique-ish per unit
#define R_GAINS     0x0A
#define R_LEVEL     0x12
#define R_EXP       0x13
#define R_BOOTS     0x15
#define R_ITEMS     0x1A
#define R_SKILLS    0x33
#define R_WEXP      0x3D
#define R_SUP_LEN   0x43
#define R_FIXED     0xBB
#define R_FLAGS     0x2A
#define R_LEARNED   0x0D
#define R_LOG       0x188
#define LOG_DATA    0x187  // logbook bytes without the trailing child marker
#define LOG_GENDER  0x1C

// Team / logbook unit ("DU") layout
#define DU_CHAR     0x01
#define DU_CLASS    0x03
#define DU_LEVEL    0x05
#define DU_BOOTS    0x06
#define DU_HIDDEN   0x07
#define DU_HAIR     0x0A
#define DU_GAINS    0x0E
#define DU_SKILLS   0x16
#define DU_WEXP     0x1B
#define DU_ITEMS    0x21
#define DU_ITEM_SZ  0x26
#define DU_LEARNED  0xE7
#define DU_LOG      0xF7
#define DU_TOTAL    (DU_LOG + LOG_DATA)
#define DU_TEAM_UNIT 0x12F                 // team units keep only name + 0x1E logbook bytes
#define DU_TEAM_HEAD 0x38
#define DU_TEAM_EXTRA (LOG_DATA - DU_TEAM_HEAD)  // profile card + messages, shared by the team
#define DU_TEAM_HDR  0x0B
#define DU_TEAM_NAME 0x2A
#define DU_TEAM_BODY (10 * DU_TEAM_UNIT + DU_TEAM_NAME + DU_TEAM_EXTRA)

static bool fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return false;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void utf16_name(const uint8_t *p, size_t bytes, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < bytes && o + 1 < outlen; i += 2) {
        uint16_t c = (uint16_t)(p[i] | p[i + 1] << 8);
        if (!c) break;
        out[o++] = c < 0x80 ? (char)c : '?';
    }
    out[o] = '\0';
}

//---------------------------------------------------------------------------
// Gathering
//---------------------------------------------------------------------------
static int add_unit_cand(const fe_save *s, const fe_unit *u, const char *source, fe_import_cand *c)
{
    if (u->size > FE_IMPORT_DATA_MAX) return 0;
    memset(c, 0, sizeof(*c));
    c->kind = FE_IMPORT_UNIT;
    c->len = (uint16_t)u->size;
    memcpy(c->data, s->data + u->off, u->size);
    for (int k = 0; k < 5; k++) {  // forged weapons only exist in their own save
        uint8_t *it = c->data + R_ITEMS + k * 5;
        int id = it[1] | it[2] << 8;
        if (fe_forge_of_item(s, id) >= 0) {
            int base = fe_item_base(s, id);
            it[1] = (uint8_t)base;
            it[2] = (uint8_t)(base >> 8);
            int max = fe_item_max_uses(s, base);
            if (it[3] > max && max) it[3] = (uint8_t)max;
        }
        it[4] &= (uint8_t)~0x20;  // "dropped by enemy"
    }
    utf16_name(s->data + u->log_off, 0x1A, c->name, sizeof(c->name));
    if (!c->name[0]) fe_unit_name(s, u, c->name, sizeof(c->name));
    snprintf(c->source, sizeof(c->source), "%s", source);
    return 1;
}

int fe_import_gather_save(const fe_save *s, const char *prefix, bool skip_army, fe_import_cand *out, int max)
{
    int n = 0;
    char src[64];
    if (!skip_army)
        for (int i = 0; i < s->unit_count && n < max; i++) {
            const fe_unit *u = &s->units[i];
            bool player = u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE || u->group == FE_GROUP_DEAD;
            if (!player || u->log_off < 0) continue;
            snprintf(src, sizeof(src), "%s, %s", prefix, u->group == FE_GROUP_DEAD ? "fallen" : "army");
            n += add_unit_cand(s, u, src, &out[n]);
        }

    // StreetPass / SpotPass teams on the world map (DU26 block)
    int b = s->is_map ? 1 : 0;
    uint32_t du = rd32(s->data + s->hdr_size + 4 + 4 * (uint32_t)(b + 5));
    uint32_t end = rd32(s->data + s->hdr_size + 4 + 4 * (uint32_t)(b + 6));
    if (!du || end <= du || end > s->size || du + 7 > end || memcmp(s->data + du, "62UD", 4) != 0)
        return n;
    const uint8_t *d = s->data;
    int teams = d[du + 6];
    uint32_t t = du + 7;
    for (int k = 0; k < teams && n < max; k++) {
        uint32_t hdr = t + 1, units = hdr + DU_TEAM_HDR;
        if (units + DU_TEAM_BODY > end) break;
        int count = d[hdr + 9];
        uint32_t team_name = units + 10 * DU_TEAM_UNIT, extra = team_name + DU_TEAM_NAME;
        char tname[24];
        utf16_name(d + team_name, DU_TEAM_NAME, tname, sizeof(tname));
        for (int j = 0; j < count && j < 10 && n < max; j++) {
            const uint8_t *u = d + units + (uint32_t)j * DU_TEAM_UNIT;
            if (!u[DU_LOG] && !u[DU_LOG + 1]) continue;  // no name: a regular character
            fe_import_cand *c = &out[n];
            memset(c, 0, sizeof(*c));
            c->kind = FE_IMPORT_DU;
            c->len = DU_TOTAL;
            memcpy(c->data, u, DU_LOG + DU_TEAM_HEAD);
            memcpy(c->data + DU_LOG + DU_TEAM_HEAD, d + extra, DU_TEAM_EXTRA);
            utf16_name(u + DU_LOG, 0x1A, c->name, sizeof(c->name));
            snprintf(c->source, sizeof(c->source), "%s, team %s", prefix, tname[0] ? tname : "?");
            n++;
        }
        t += 1 + DU_TEAM_HDR + DU_TEAM_BODY;
    }
    return n;
}

int fe_import_gather_logbook(const uint8_t *g, size_t size, fe_import_cand *out, int max)
{
    if (size < 12) return 0;
    uint32_t off = rd32(g + 8);
    if (off + 12 > size || memcmp(g + off, "81GD", 4) != 0) return 0;
    int count = g[off + 0xA], n = 0;
    uint32_t e = off + 0xC;
    for (int k = 0; k < count && n < max; k++, e += 5 + DU_TOTAL) {
        if (e + 5 + DU_TOTAL > size) break;
        const uint8_t *u = g + e + 5;
        fe_import_cand *c = &out[n];
        memset(c, 0, sizeof(*c));
        c->kind = FE_IMPORT_DU;
        c->len = DU_TOTAL;
        memcpy(c->data, u, DU_TOTAL);
        utf16_name(u + DU_LOG, 0x1A, c->name, sizeof(c->name));
        if (!c->name[0]) continue;
        snprintf(c->source, sizeof(c->source), "Logbook #%d", k + 1);
        n++;
    }
    return n;
}

//---------------------------------------------------------------------------
// Building the record
//---------------------------------------------------------------------------
// Drops the support bytes and any child data; returns the logbook offset (or 0) and the
// flags offset. *len is updated.
static uint32_t strip_record(uint8_t *r, uint32_t *len, uint32_t *flags)
{
    uint32_t sup = r[R_SUP_LEN];
    memmove(r + R_SUP_LEN + 1, r + R_SUP_LEN + 1 + sup, *len - (R_SUP_LEN + 1 + sup));
    *len -= sup;
    r[R_SUP_LEN] = 0;
    uint32_t unk = r[R_SUP_LEN + 1];
    *flags = R_SUP_LEN + 2 + unk;
    uint32_t fixed = R_FIXED + unk;
    if (fixed > *len || r[fixed - 2] != 1 || r[fixed - 1] != 6 || fixed + R_LOG > *len)
        return 0;
    r[fixed + LOG_DATA] = 0;  // no child data follows
    *len = fixed + R_LOG;
    return fixed;
}

static void clean_flags(uint8_t *f)
{
    f[2] = 0xFF;                 // no deployment slot
    f[0x0F] &= (uint8_t)~0x8F;   // moved, paired up, fallen, fell on this map
    f[0x10] &= (uint8_t)~0x05;   // foreign unit, selected for battle
    f[0x11] &= (uint8_t)~0x40;   // married to the Maiden
    f[0x12] &= (uint8_t)~0x20;   // wireless unit
}

static void set_avatar_char(uint8_t *r, uint32_t log, bool supports)
{
    int old = r[R_CHAR] | r[R_CHAR + 1] << 8;
    int now;
    if (supports) {
        if (old == 0 || old == 1) return;
        now = r[log + LOG_GENDER] ? 1 : 0;
    } else {
        if (old != 0 && old != 1) return;
        now = 2;  // logbook unit: same personal bases as the Avatar, no supports
    }
    for (int k = 0; k < FE_STAT_COUNT; k++) {  // keep the displayed stats (gains are 8-bit, wrapping)
        int add_old = fe_char(old) ? fe_char(old)->additions[k] : 0;
        int add_now = fe_char(now) ? fe_char(now)->additions[k] : 0;
        int g = (int8_t)r[R_GAINS + k] + add_old - add_now;
        r[R_GAINS + k] = (uint8_t)(g < -128 ? -128 : g > 255 ? 255 : g);
    }
    r[R_CHAR] = (uint8_t)now;
    r[R_CHAR + 1] = 0;
}

static int find_template(const fe_save *s)
{
    int best = -1;
    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        if (u->log_off < 0) continue;
        bool player = u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE || u->group == FE_GROUP_DEAD;
        uint16_t c = fe_unit_char_id(s, u);
        if (player && (c == 0 || c == 1)) return i;
        if (best < 0) best = i;
    }
    return best;
}

int fe_import_add(fe_save *dst, const fe_import_cand *c, bool supports, char *err, size_t errlen)
{
    uint8_t r[FE_IMPORT_DATA_MAX + 64];
    uint32_t len, flags, log;
    int sup_char = -1, sup_n = 0;
    uint8_t sup[256];
    if (c->kind == FE_IMPORT_UNIT) {
        len = c->len;
        memcpy(r, c->data, len);
        int ch = r[R_CHAR] | r[R_CHAR + 1] << 8;
        if (fe_char_supports(ch) && fe_char_supports(ch)->count) {  // supports are listed per character
            sup_char = ch;
            sup_n = r[R_SUP_LEN];
            memcpy(sup, r + R_SUP_LEN + 1, (size_t)sup_n);
        }
        log = strip_record(r, &len, &flags);
        if (!log) { fail(err, errlen, "this unit has no Avatar data"); return -1; }
    } else {
        int t = find_template(dst);
        if (t < 0) { fail(err, errlen, "this save has no Avatar to use as a template"); return -1; }
        const fe_unit *tu = &dst->units[t];
        if (tu->size > FE_IMPORT_DATA_MAX) { fail(err, errlen, "template unit too large"); return -1; }
        len = tu->size;
        memcpy(r, dst->data + tu->off, len);
        log = strip_record(r, &len, &flags);
        if (!log) { fail(err, errlen, "template has no Avatar data"); return -1; }
        const uint8_t *u = c->data;
        r[R_CHAR] = u[DU_CHAR];
        r[R_CHAR + 1] = u[DU_CHAR + 1];
        r[R_CLASS] = u[DU_CLASS];
        r[R_LEVEL] = u[DU_LEVEL] ? u[DU_LEVEL] : 1;
        r[R_EXP] = 0;
        r[R_BOOTS] = u[DU_BOOTS];
        r[flags] = u[DU_HIDDEN];
        memcpy(r + R_GAINS, u + DU_GAINS, FE_STAT_COUNT);
        for (int k = 0; k < 5; k++) {
            r[R_SKILLS + 2 * k] = u[DU_SKILLS + k];
            r[R_SKILLS + 2 * k + 1] = 0;
        }
        memcpy(r + R_WEXP, u + DU_WEXP, 6);
        int slot = 0;
        for (int k = 0; k < 5; k++) {
            uint8_t *it = r + R_ITEMS + k * 5;
            it[0] = 0x04; it[1] = it[2] = it[3] = it[4] = 0;
        }
        for (int k = 0; k < 5; k++) {
            int id = u[DU_ITEMS + k * DU_ITEM_SZ];
            if (!id || !fe_item(id)) continue;
            uint8_t *it = r + R_ITEMS + slot++ * 5;
            it[1] = (uint8_t)id;
            it[3] = (uint8_t)fe_item_max_uses(dst, id);
        }
        memcpy(r + flags + R_FLAGS, u + DU_LEARNED, R_LEARNED);
        uint8_t *end = r + flags + R_FLAGS + R_LEARNED;
        memset(end + 0x31, 0, 4);              // battles, victories
        memcpy(end + 0x39, u + DU_HAIR, 4);    // hair color (unit copy)
        memcpy(r + log, u + DU_LOG, LOG_DATA);
        r[log + LOG_DATA] = 0;
    }
    clean_flags(r + flags);
    set_avatar_char(r, log, supports);
    static uint32_t salt = 0x7F4A7C15u;  // a fresh per-unit id
    salt = salt * 1664525u + 1013904223u + dst->size + len;
    r[R_UNIQUE] = (uint8_t)salt; r[R_UNIQUE + 1] = (uint8_t)(salt >> 8);
    r[R_UNIQUE + 2] = (uint8_t)(salt >> 16); r[R_UNIQUE + 3] = (uint8_t)(salt >> 24);

    // Is there already a unit of this character? Then partner sides are shared with it.
    int now_char = r[R_CHAR] | r[R_CHAR + 1] << 8;
    bool shared = false;
    for (int i = 0; i < dst->unit_count; i++)
        if (fe_unit_char_id(dst, &dst->units[i]) == now_char) shared = true;

    int ni = fe_unit_insert(dst, r, len, err, errlen);
    if (ni < 0) return -1;
    fe_unit_heal(dst, &dst->units[ni]);
    if (supports && sup_char >= 0 && sup_char == now_char) {
        const fe_support_list *sl = fe_char_supports(sup_char);
        for (int k = 0; sl && k < sup_n && k < sl->count; k++) {
            int v = sup[k];
            if (!v || sl->partners[k].partner == FE_SUPPORT_EMPTY || sl->partners[k].type > 3) continue;
            int a = fe_support_a_rank(sl->partners[k].type);
            if (v > a) v = a;
            char e2[160];
            bool ok = shared ? fe_support_set_own(dst, ni, sl->partners[k].partner, v, e2, sizeof(e2))
                             : fe_support_set(dst, ni, sl->partners[k].partner, v, e2, sizeof(e2));
            if (!ok) break;  // the unit is in; supports are a bonus
        }
    }
    return ni;
}

int fe_import_gather_heroes(fe_import_cand *out, int max)
{
    int n = 0;
    for (int k = 0; k < FE_LOOK_PRESET_COUNT && n < max; k++) {
        const fe_look_preset *p = &fe_look_presets[k];
        const fe_hero_unit *h = &fe_hero_units[k];
        fe_import_cand *c = &out[n];
        memset(c, 0, sizeof(*c));
        c->kind = FE_IMPORT_DU;
        c->len = DU_TOTAL;
        uint8_t *u = c->data;
        int hc = fe_char_by_vanilla_id(h->char_id);  // Marth is 52 in vanilla, 62 in Thabes' mod
        u[DU_CHAR] = (uint8_t)(hc >= 0 ? hc : h->char_id);
        u[DU_CLASS] = h->cls;
        u[DU_LEVEL] = h->level ? h->level : 1;
        u[DU_HAIR] = (uint8_t)(p->color >> 16); u[DU_HAIR + 1] = (uint8_t)(p->color >> 8);
        u[DU_HAIR + 2] = (uint8_t)p->color; u[DU_HAIR + 3] = 0xFF;
        memcpy(u + DU_GAINS, h->gains, 8);
        memcpy(u + DU_SKILLS, h->skills, 5);
        memcpy(u + DU_WEXP, h->wexp, 6);
        for (int i = 0; i < 5; i++) {
            u[DU_ITEMS + i * DU_ITEM_SZ] = h->items[i];
            if (h->skills[i]) u[DU_LEARNED + h->skills[i] / 8] |= (uint8_t)(1 << (h->skills[i] % 8));
        }
        uint8_t *log = u + DU_LOG;
        const char *nm = p->name;
        for (int i = 0; nm[i] && i < 12; i++) log[i * 2] = (uint8_t)nm[i];
        log[0x1A] = h->asset;
        log[0x1B] = h->flaw;
        log[LOG_GENDER] = p->gender;
        log[0x1D] = p->build;
        log[0x1E] = p->face;
        log[0x1F] = p->hair;
        log[0x20] = (uint8_t)(p->color >> 16); log[0x21] = (uint8_t)(p->color >> 8);
        log[0x22] = (uint8_t)p->color; log[0x23] = 0xFF;
        log[0x24] = p->voice;
        log[0x27] = p->log_id;  // with the hero flag, the game shows this hero's own portrait
        log[0x37] = 0x02;       // hero flag (set on every SpotPass/DLC unit in real logbooks)
        log[0x39] = h->cls;     // real logbook entries repeat the class here
        snprintf(c->name, sizeof(c->name), "%s", p->name);
        // logbook ids 200+ are the paid DLC heroes (join at level 1); the rest came by SpotPass
        snprintf(c->source, sizeof(c->source), "%s", p->log_id >= 200 ? "DLC" : "SpotPass");
        n++;
    }
    return n;
}
