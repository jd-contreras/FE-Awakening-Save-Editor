// Romfs mod data (see femod.h). Formats, checked against vanilla USA v1.0 and Thabes' mod:
//  - IS binary: u32 file size, data size, pointer count, label count; data at 0x20; then the
//    pointer table (data offsets of pointer fields), labels (u32 data offset, u32 name offset),
//    strings. Pointer values are data offsets.
//  - data/person/static.bin: u32 count at data+4, 0x238-byte records from data+8. +0 bit 0
//    female, +8 PID, +0x10 default class JID, +0x14 name label, +0x1C base additions (8 x s8),
//    +0x34 cap modifiers, +0x48 personal skills, +0x88 52 support slots [PID ptr][C B A S].
//  - data/GameData.bin: item records (IID_ labels, 0x38 bytes): +0x0C name label, +0x18 type,
//    +0x1F uses. IID_REFINE* are the forge slots, so the first one ends the regular items.
//    Class records (JID_ labels, 0x80 bytes): +0 bit 3 promoted, +0x10 name label, +0x18 bases
//    (s8), +0x30 caps.
//  - m/E/GameData.bin: labels -> UTF-16 text (MPID_/MIID_/MJID_ names).
#include "core/femod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *const fe_vanilla_pids[FE_CHAR_COUNT];

#define PERSON_SIZE 0x238
#define PERSON_SUPPORTS 0x88
#define MAX_SLOTS 52
#define NAME_MAX_LEN 28

static bool fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return false;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

//---------------------------------------------------------------------------
// LZ11 / LZ13
//---------------------------------------------------------------------------
bool fe_lz_decompress(const uint8_t *in, size_t n, uint8_t **out, size_t *outn)
{
    *out = NULL;
    size_t p = 0;
    if (n >= 8 && in[0] == 0x13) p = 4;  // LZ13 wrapper
    if (p + 4 > n || in[p] != 0x11) return false;
    size_t size = in[p + 1] | in[p + 2] << 8 | (size_t)in[p + 3] << 16;
    p += 4;
    if (size == 0) {
        if (p + 4 > n) return false;
        size = rd32(in + p);
        p += 4;
    }
    if (size == 0 || size > 64u << 20) return false;
    uint8_t *o = malloc(size);
    if (!o) return false;
    size_t w = 0;
    while (w < size) {
        if (p >= n) goto bad;
        uint8_t flags = in[p++];
        for (int bit = 7; bit >= 0 && w < size; bit--) {
            if (!((flags >> bit) & 1)) {
                if (p >= n) goto bad;
                o[w++] = in[p++];
                continue;
            }
            if (p + 2 > n) goto bad;
            uint8_t b = in[p];
            size_t len, disp;
            if ((b >> 4) == 0) {
                if (p + 3 > n) goto bad;
                len = (((size_t)(b & 0xF) << 4) | (in[p + 1] >> 4)) + 0x11;
                disp = (((size_t)(in[p + 1] & 0xF) << 8) | in[p + 2]) + 1;
                p += 3;
            } else if ((b >> 4) == 1) {
                if (p + 4 > n) goto bad;
                len = (((size_t)(b & 0xF) << 12) | ((size_t)in[p + 1] << 4) | (in[p + 2] >> 4)) + 0x111;
                disp = (((size_t)(in[p + 2] & 0xF) << 8) | in[p + 3]) + 1;
                p += 4;
            } else {
                len = (b >> 4) + 1;
                disp = (((size_t)(b & 0xF) << 8) | in[p + 1]) + 1;
                p += 2;
            }
            if (disp > w) goto bad;
            for (size_t i = 0; i < len && w < size; i++, w++)
                o[w] = o[w - disp];
        }
    }
    *out = o;
    *outn = size;
    return true;
bad:
    free(o);
    return false;
}

static bool read_lz_file(const char *path, uint8_t **out, size_t *outn, bool *missing)
{
    *missing = false;
    FILE *f = fopen(path, "rb");
    if (!f) { *missing = true; return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = raw && fread(raw, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    ok = ok && fe_lz_decompress(raw, (size_t)n, out, outn);
    free(raw);
    return ok;
}

//---------------------------------------------------------------------------
// IS binary
//---------------------------------------------------------------------------
typedef struct {
    const uint8_t *d;
    size_t n, data, ptab, ltab, strtab;
    uint32_t ds, p1, p2;
    uint32_t *ptrs;  // sorted pointer field offsets
} isbin;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static bool is_open(isbin *b, const uint8_t *d, size_t n)
{
    memset(b, 0, sizeof(*b));
    if (n < 0x20) return false;
    b->d = d; b->n = n;
    b->ds = rd32(d + 4); b->p1 = rd32(d + 8); b->p2 = rd32(d + 12);
    b->data = 0x20;
    b->ptab = b->data + b->ds;
    b->ltab = b->ptab + (size_t)b->p1 * 4;
    b->strtab = b->ltab + (size_t)b->p2 * 8;
    if (b->strtab > n) return false;
    b->ptrs = malloc((b->p1 ? b->p1 : 1) * sizeof(uint32_t));
    if (!b->ptrs) return false;
    for (uint32_t i = 0; i < b->p1; i++) b->ptrs[i] = rd32(d + b->ptab + 4 * i);
    qsort(b->ptrs, b->p1, sizeof(uint32_t), cmp_u32);
    return true;
}

static void is_close(isbin *b) { free(b->ptrs); b->ptrs = NULL; }

static bool is_ptr_field(const isbin *b, uint32_t off)
{
    return bsearch(&off, b->ptrs, b->p1, sizeof(uint32_t), cmp_u32) != NULL;
}

// String pointed to by the pointer field at data offset `off` (NULL if not a string pointer).
static const char *is_str_field(const isbin *b, uint32_t off)
{
    if (off + 4 > b->ds || !is_ptr_field(b, off)) return NULL;
    size_t a = b->data + rd32(b->d + b->data + off);
    if (a < b->strtab || a >= b->n || !memchr(b->d + a, 0, b->n - a)) return NULL;
    return (const char *)b->d + a;
}

static const uint8_t *is_rec(const isbin *b, uint32_t off, uint32_t size)
{
    return (size_t)off + size <= b->ds ? b->d + b->data + off : NULL;
}

typedef struct { uint32_t addr; const char *name; } is_label;

static int cmp_label_addr(const void *a, const void *b)
{
    const is_label *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int cmp_label_name(const void *a, const void *b)
{
    return strcmp(((const is_label *)a)->name, ((const is_label *)b)->name);
}

// Labels whose name starts with `prefix`, sorted by address. *out is malloc'd.
static int is_labels(const isbin *b, const char *prefix, is_label **out)
{
    size_t pl = strlen(prefix);
    *out = malloc((b->p2 ? b->p2 : 1) * sizeof(is_label));
    if (!*out) return -1;
    int c = 0;
    for (uint32_t i = 0; i < b->p2; i++) {
        uint32_t addr = rd32(b->d + b->ltab + 8 * i);
        size_t s = b->strtab + rd32(b->d + b->ltab + 8 * i + 4);
        if (s >= b->n || !memchr(b->d + s, 0, b->n - s)) continue;
        const char *name = (const char *)b->d + s;
        if (strncmp(name, prefix, pl) == 0) (*out)[c++] = (is_label){addr, name};
    }
    qsort(*out, c, sizeof(is_label), cmp_label_addr);
    return c;
}

//---------------------------------------------------------------------------
// Names (m/E/GameData.bin)
//---------------------------------------------------------------------------
typedef struct {
    uint8_t *buf;
    isbin bin;
    is_label *labels;  // sorted by name
    int count;
} msgs;

static void msgs_free(msgs *m)
{
    is_close(&m->bin);
    free(m->labels);
    free(m->buf);
    memset(m, 0, sizeof(*m));
}

static bool msgs_load(msgs *m, const char *path)
{
    memset(m, 0, sizeof(*m));
    size_t n;
    bool missing;
    if (!read_lz_file(path, &m->buf, &n, &missing)) return false;
    if (!is_open(&m->bin, m->buf, n)) { msgs_free(m); return false; }
    m->count = is_labels(&m->bin, "", &m->labels);
    if (m->count < 0) { msgs_free(m); return false; }
    qsort(m->labels, m->count, sizeof(is_label), cmp_label_name);
    return true;
}

static char *pool_add(femod *fm, const char *s)
{
    size_t len = strlen(s);
    if (len > NAME_MAX_LEN) len = NAME_MAX_LEN;
    if (fm->strings_len + len + 1 > fm->strings_cap) return (char *)"?";
    char *p = fm->strings + fm->strings_len;
    memcpy(p, s, len);
    p[len] = 0;
    fm->strings_len += len + 1;
    return p;
}

// UTF-8 name for message `label`, or "" if unknown.
static void msgs_name(const msgs *m, const char *label, char *out, size_t outlen)
{
    out[0] = 0;
    if (!m->count || !label) return;
    is_label key = {0, label};
    const is_label *l = bsearch(&key, m->labels, m->count, sizeof(is_label), cmp_label_name);
    if (!l) return;
    size_t a = m->bin.data + l->addr, o = 0;
    while (a + 1 < m->bin.n && o + 4 < outlen) {
        unsigned c = m->bin.d[a] | m->bin.d[a + 1] << 8;
        a += 2;
        if (!c || c == '\n') break;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

//---------------------------------------------------------------------------
// Loading
//---------------------------------------------------------------------------
static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && memcmp(s + a - b, suffix, b) == 0;
}

static const char SJIS_MALE[] = "\x92\x6A", SJIS_FEMALE[] = "\x8F\x97";

static int support_type(const uint8_t t[4])
{
    static const uint8_t T[4][4] = {{4, 10, 18, 99}, {5, 10, 16, 22}, {4, 9, 14, 20}, {3, 8, 13, 18}};
    if (t[0] == 99 && t[1] == 99 && t[2] == 99) return 4;  // no reachable rank
    for (int i = 0; i < 4; i++)
        if (!memcmp(t, T[i], 4)) return i;
    if (t[3] == 99) return 0;
    return t[3] >= 22 ? 1 : t[3] >= 20 ? 2 : 3;
}

static bool load_gamedata(femod *fm, const isbin *b, const msgs *names, char *err, size_t errlen,
                          is_label **jids_out, int *jid_count)
{
    is_label *iids = NULL, *jids = NULL;
    int ni = is_labels(b, "IID_", &iids), nj = is_labels(b, "JID_", &jids);
    if (ni <= 0 || nj <= 0) { free(iids); free(jids); return fail(err, errlen, "GameData: no item or class table"); }

    int regular = ni;
    for (int i = 0; i < ni; i++)
        if (!strncmp(iids[i].name, "IID_REFINE", 10)) { regular = i; break; }
    fm->items = calloc(regular, sizeof(fe_item_info));
    fm->classes = calloc(nj, sizeof(fe_class_info));
    if (!fm->items || !fm->classes) { free(iids); free(jids); return fail(err, errlen, "out of memory"); }

    char name[64];
    for (int i = 0; i < regular; i++) {
        const uint8_t *r = is_rec(b, iids[i].addr, 0x38);
        if (!r) { free(iids); free(jids); return fail(err, errlen, "GameData: item record out of range"); }
        msgs_name(names, is_str_field(b, iids[i].addr + 0x0C), name, sizeof(name));
        if (!name[0] && i < FE_ITEM_COUNT) snprintf(name, sizeof(name), "%s", fe_items[i].name);
        if (!name[0]) snprintf(name, sizeof(name), "Item %d", i);
        if (i == 0) snprintf(name, sizeof(name), "None");
        fm->items[i].name = pool_add(fm, name);
        fm->items[i].type = r[0x18] <= 10 ? r[0x18] : 10;
        fm->items[i].uses = r[0x1F];
    }
    fm->item_count = regular;

    for (int i = 0; i < nj; i++) {
        const uint8_t *r = is_rec(b, jids[i].addr, 0x80);
        if (!r) { free(iids); free(jids); return fail(err, errlen, "GameData: class record out of range"); }
        fe_class_info *c = &fm->classes[i];
        for (int k = 0; k < FE_STAT_COUNT; k++) {
            c->base[k] = (int8_t)r[0x18 + k];
            c->max[k] = r[0x30 + k];
        }
        c->promoted = (r[0] >> 3) & 1;
        const char *jid = jids[i].name;
        bool male = ends_with(jid, SJIS_MALE), female = ends_with(jid, SJIS_FEMALE);
        if (i < FE_CLASS_COUNT) {
            c->name = fe_classes[i].name;
            c->female = fe_classes[i].female;
            c->enemy_only = fe_classes[i].enemy_only;
        } else {
            msgs_name(names, is_str_field(b, jids[i].addr + 0x10), name, sizeof(name));
            if (!name[0]) snprintf(name, sizeof(name), "Class %d", i);
            // "JID_<class>2男": a second version of an existing class
            size_t jl = strlen(jid);
            char digit = (male || female) && jl >= 3 && jid[jl - 3] >= '2' && jid[jl - 3] <= '9' ? jid[jl - 3] : 0;
            char num[4] = "";
            if (digit) snprintf(num, sizeof(num), " %c", digit);
            char full[80];
            snprintf(full, sizeof(full), "%s%s%s", name, num, male ? " (M)" : female ? " (F)" : "");
            c->name = pool_add(fm, full);
            c->female = female;
            c->enemy_only = 0;
        }
    }
    fm->class_count = nj;
    free(iids);
    *jids_out = jids;
    *jid_count = nj;
    return true;
}

static int class_by_jid(const is_label *jids, int nj, const char *jid)
{
    if (!jid) return -1;
    for (int i = 0; i < nj; i++)
        if (!strcmp(jids[i].name, jid)) return i;
    return -1;
}

static bool load_persons(femod *fm, const isbin *b, const msgs *names, const is_label *jids, int nj,
                         char *err, size_t errlen)
{
    uint32_t count = rd32(b->d + b->data + 4);
    if (count == 0 || count > 250 || 8 + (size_t)count * PERSON_SIZE > b->ds)
        return fail(err, errlen, "static.bin: bad character count");
    fm->char_count = (int)count;
    fm->chars = calloc(count, sizeof(fe_char_info));
    fm->recruit = calloc(count, sizeof(fe_char_recruit));
    fm->supports = calloc(count, sizeof(fe_support_list));
    fm->char_vanilla = calloc(count, sizeof(int16_t));
    const char **pids = calloc(count, sizeof(char *));
    if (!fm->chars || !fm->recruit || !fm->supports || !fm->char_vanilla || !pids) {
        free(pids);
        return fail(err, errlen, "out of memory");
    }
    for (uint32_t i = 0; i < count; i++) pids[i] = is_str_field(b, 8 + i * PERSON_SIZE + 8);

    char name[64];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = 8 + i * PERSON_SIZE;
        const uint8_t *r = b->d + b->data + off;
        int vid = -1;
        for (int v = 0; v < FE_CHAR_COUNT && pids[i]; v++)
            if (!strcmp(pids[i], fe_vanilla_pids[v])) { vid = v; break; }
        fm->char_vanilla[i] = (int16_t)vid;

        fe_char_info *c = &fm->chars[i];
        if (vid >= 0) {
            c->name = fe_chars[vid].name;
        } else {
            msgs_name(names, is_str_field(b, off + 0x14), name, sizeof(name));
            if (!name[0]) snprintf(name, sizeof(name), "Character %u", (unsigned)i);
            c->name = pool_add(fm, name);
        }
        for (int k = 0; k < FE_STAT_COUNT; k++) {
            c->additions[k] = (int8_t)r[0x1C + k];
            c->modifiers[k] = (int8_t)r[0x34 + k];
        }

        fe_char_recruit *rc = &fm->recruit[i];
        if (vid >= 0) {
            *rc = fe_recruit[vid];
        } else {
            rc->female = r[0] & 1;
            rc->parent = -1;
            int cls = class_by_jid(jids, nj, is_str_field(b, off + 0x10));
            rc->start_class = cls >= 0 && cls < 256 ? (uint8_t)cls : 0;
            for (int k = 0; k < 5; k++) rc->skills[k] = r[0x48 + k] < FE_SKILL_COUNT ? r[0x48 + k] : 0;
        }

        fe_support_list *sl = &fm->supports[i];
        for (int k = 0; k < MAX_SLOTS && k < FE_MAX_SUPPORTS; k++) {
            uint32_t f = off + PERSON_SUPPORTS + 8 * k;
            const char *partner = is_str_field(b, f);
            sl->partners[k].partner = FE_SUPPORT_EMPTY;
            if (!partner) continue;
            for (uint32_t p = 0; p < count; p++)
                if (pids[p] && !strcmp(pids[p], partner)) { sl->partners[k].partner = (uint8_t)p; break; }
            if (sl->partners[k].partner == FE_SUPPORT_EMPTY) continue;
            sl->partners[k].type = (uint8_t)support_type(r + PERSON_SUPPORTS + 8 * k + 4);
            sl->count = (uint8_t)(k + 1);
        }
    }
    free(pids);
    // the mod can reuse a name label (two "Raimi"): number the later ones
    for (uint32_t i = 0; i < count; i++) {
        if (fm->char_vanilla[i] >= 0) continue;
        int dup = 1;
        for (uint32_t j = 0; j < i; j++)
            if (!strcmp(fm->chars[j].name, fm->chars[i].name)) dup++;
        if (dup > 1) {
            char nm[48];
            snprintf(nm, sizeof(nm), "%s %d", fm->chars[i].name, dup);
            fm->chars[i].name = pool_add(fm, nm);
        }
    }
    return true;
}

bool femod_load(femod *fm, const char *romfs_dir, char *err, size_t errlen)
{
    memset(fm, 0, sizeof(*fm));
    char path[256];
    uint8_t *gd = NULL, *ps = NULL;
    size_t gdn = 0, psn = 0;
    bool missing, ok = true;
    msgs names;
    memset(&names, 0, sizeof(names));

    snprintf(path, sizeof(path), "%s/m/E/GameData.bin.lz", romfs_dir);
    fm->has_names = msgs_load(&names, path);
    if (!fm->has_names) {
        snprintf(path, sizeof(path), "%s/m/U/GameData.bin.lz", romfs_dir);
        fm->has_names = msgs_load(&names, path);
    }

    snprintf(path, sizeof(path), "%s/data/GameData.bin.lz", romfs_dir);
    bool gd_ok = read_lz_file(path, &gd, &gdn, &missing);
    if (!gd_ok && !missing) { ok = fail(err, errlen, "data/GameData.bin.lz could not be read"); goto done; }
    snprintf(path, sizeof(path), "%s/data/person/static.bin.lz", romfs_dir);
    bool ps_ok = read_lz_file(path, &ps, &psn, &missing);
    if (!ps_ok && !missing) { ok = fail(err, errlen, "data/person/static.bin.lz could not be read"); goto done; }
    if (!gd_ok && !ps_ok) goto done;  // no data files: vanilla

    fm->strings_cap = 64 * 1024;
    fm->strings = malloc(fm->strings_cap);
    if (!fm->strings) { ok = fail(err, errlen, "out of memory"); goto done; }

    is_label *jids = NULL;
    int nj = 0;
    if (gd_ok) {
        isbin b;
        if (!is_open(&b, gd, gdn)) { ok = fail(err, errlen, "GameData: bad format"); goto done; }
        ok = load_gamedata(fm, &b, &names, err, errlen, &jids, &nj);
        if (ok) {
            // keep the JID names alive for load_persons: they point into gd
            fm->has_gamedata = true;
        }
        if (ok && ps_ok) {
            isbin pb;
            if (!is_open(&pb, ps, psn)) ok = fail(err, errlen, "static.bin: bad format");
            else {
                ok = load_persons(fm, &pb, &names, jids, nj, err, errlen);
                fm->has_persons = ok;
                is_close(&pb);
            }
        }
        is_close(&b);
    } else {
        isbin pb;
        if (!is_open(&pb, ps, psn)) ok = fail(err, errlen, "static.bin: bad format");
        else {
            ok = load_persons(fm, &pb, &names, NULL, 0, err, errlen);
            fm->has_persons = ok;
            is_close(&pb);
        }
    }
    free(jids);
    if (!ok) goto done;

    fm->active = fm->has_gamedata || fm->has_persons;
    snprintf(fm->label, sizeof(fm->label), "Modded");

done:
    msgs_free(&names);
    free(gd);
    free(ps);
    if (!ok) femod_free(fm);
    return ok;
}

bool femod_differs_from_vanilla(const femod *m)
{
    if (m->has_persons) {
        if (m->char_count != FE_CHAR_COUNT) return true;
        for (int i = 0; i < FE_CHAR_COUNT; i++) {
            if (m->char_vanilla[i] != i) return true;
            if (memcmp(m->chars[i].additions, fe_chars[i].additions, FE_STAT_COUNT) ||
                memcmp(m->chars[i].modifiers, fe_chars[i].modifiers, FE_STAT_COUNT)) return true;
            const fe_support_list *a = &m->supports[i], *b = &fe_supports[i];
            if (a->count != b->count) return true;
            for (int k = 0; k < a->count; k++)
                if (a->partners[k].partner != b->partners[k].partner || a->partners[k].type != b->partners[k].type)
                    return true;
        }
    }
    if (m->has_gamedata) {
        if (m->class_count != FE_CLASS_COUNT || m->item_count != FE_ITEM_COUNT) return true;
        for (int i = 0; i < FE_CLASS_COUNT; i++) {
            // the editor's table differs from the game here (Thief caps, Taguel (F) bases)
            bool skip_caps = i == 34 || i == 35, skip_bases = i == 70;
            if (!skip_bases && memcmp(m->classes[i].base, fe_classes[i].base, FE_STAT_COUNT)) return true;
            if (!skip_caps && memcmp(m->classes[i].max, fe_classes[i].max, FE_STAT_COUNT)) return true;
        }
        for (int i = 1; i < FE_ITEM_COUNT; i++)
            if (m->items[i].type != fe_items[i].type || m->items[i].uses != fe_items[i].uses) return true;
    }
    return false;
}

void femod_free(femod *fm)
{
    if (femod_active() == fm) femod_set_active(NULL);
    free(fm->chars); free(fm->recruit); free(fm->supports); free(fm->char_vanilla);
    free(fm->items); free(fm->classes); free(fm->strings);
    memset(fm, 0, sizeof(*fm));
}

//---------------------------------------------------------------------------
// Active data set
//---------------------------------------------------------------------------
static const femod *g_mod;

void femod_set_active(const femod *m) { g_mod = m && m->active ? m : NULL; }
const femod *femod_active(void) { return g_mod; }

static bool mod_persons(void) { return g_mod && g_mod->has_persons; }
static bool mod_gamedata(void) { return g_mod && g_mod->has_gamedata; }

int fe_char_count(void) { return mod_persons() ? g_mod->char_count : FE_CHAR_COUNT; }

const fe_char_info *fe_char(int id)
{
    if (id < 0 || id >= fe_char_count()) return NULL;
    return mod_persons() ? &g_mod->chars[id] : &fe_chars[id];
}

const fe_char_recruit *fe_char_recruit_of(int id)
{
    if (id < 0 || id >= fe_char_count()) return NULL;
    return mod_persons() ? &g_mod->recruit[id] : &fe_recruit[id];
}

const fe_support_list *fe_char_supports(int id)
{
    if (id < 0 || id >= fe_char_count()) return NULL;
    return mod_persons() ? &g_mod->supports[id] : &fe_supports[id];
}

int fe_char_vanilla_id(int id)
{
    if (id < 0 || id >= fe_char_count()) return -1;
    return mod_persons() ? g_mod->char_vanilla[id] : id;
}

int fe_char_by_vanilla_id(int vanilla_id)
{
    if (!mod_persons()) return vanilla_id;
    for (int i = 0; i < g_mod->char_count; i++)
        if (g_mod->char_vanilla[i] == vanilla_id) return i;
    return -1;
}

int fe_item_count(void) { return mod_gamedata() ? g_mod->item_count : FE_ITEM_COUNT; }

const fe_item_info *fe_item(int id)
{
    if (id < 0 || id >= fe_item_count()) return NULL;
    return mod_gamedata() ? &g_mod->items[id] : &fe_items[id];
}

int fe_class_count(void) { return mod_gamedata() ? g_mod->class_count : FE_CLASS_COUNT; }

const fe_class_info *fe_class(int id)
{
    if (id < 0 || id >= fe_class_count()) return NULL;
    return mod_gamedata() ? &g_mod->classes[id] : &fe_classes[id];
}
