#include "core/feglobal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/crc32.h"
#include "core/huffman.h"

#define COMP_HDR 0x10

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

static bool parse(fe_global *g, char *err, size_t errlen)
{
    if (g->size < 0x50 || memcmp(g->data, "EDNI", 4) != 0)
        return fail(err, errlen, "Global: block index (INDE) not found");
    uint32_t user = rd32(g->data + 4), next = rd32(g->data + 8);
    if (user < 8 || next <= user || next > g->size || memcmp(g->data + user, "RESU", 4) != 0)
        return fail(err, errlen, "Global: USER block not where expected");

    uint32_t o = user + 4;
    g->flags_off = o + 1;
    o += 0x0D;
    uint32_t *offs[3] = {&g->gallery_off, &g->support_off, &g->colors_off};
    uint32_t *bits[3] = {&g->gallery_bits, &g->support_bits, &g->colors_bits};
    static const uint32_t need[3] = {FE_GALLERY_ENTRIES, FE_SUPPORT_LOG_ENTRIES, FE_HAIR_FLAG_ENTRIES};
    for (int i = 0; i < 3; i++) {
        if (o + 4 > next)
            return fail(err, errlen, "Global: flag list %d truncated", i);
        uint32_t count = rd32(g->data + o);
        o += 4;
        uint32_t bytes = count / 8 + 1;
        if (o + bytes > next || count < need[i])
            return fail(err, errlen, "Global: flag list %d has %u entries, expected at least %u",
                        i, (unsigned)count, (unsigned)need[i]);
        *offs[i] = o;
        *bits[i] = count;
        o += bytes;
    }
    // unknown flags (u32 count + count/8 bytes), then the two Avatar profiles
    if (o + 4 > next)
        return fail(err, errlen, "Global: unknown flag list truncated");
    uint32_t ucount = rd32(g->data + o);
    o += 4 + ucount / 8;
    if (o + 2 * (1 + FE_GLOBAL_AVATAR_SIZE) > next || g->data[o] != 0x06 ||
        g->data[o + 1 + FE_GLOBAL_AVATAR_SIZE] != 0x06)
        return fail(err, errlen, "Global: Avatar profiles not where expected");
    g->avatar_off = o;
    return true;
}

bool fe_global_load(fe_global *g, const uint8_t *file, size_t len, char *err, size_t errlen)
{
    memset(g, 0, sizeof(*g));
    if (len < COMP_HDR + 8 || memcmp(file, "PMOC", 4) != 0 || rd32(file + 4) != 2)
        return fail(err, errlen, "Global: not a compressed Global file");
    uint32_t declen = rd32(file + 8), crc = rd32(file + 12);
    if (declen < 0x50 || declen > 1024 * 1024)
        return fail(err, errlen, "Global: implausible size %u", (unsigned)declen);
    g->data = malloc(declen);
    if (!g->data)
        return fail(err, errlen, "Global: out of memory");
    g->size = declen;
    huff_result hr = huff_decompress(file + COMP_HDR, len - COMP_HDR, g->data, declen);
    if (hr != HUFF_OK) {
        fail(err, errlen, "Global: decompression failed: %s", huff_strerror(hr));
        goto bad;
    }
    if (fe_crc32(0, g->data, declen) != crc) {
        fail(err, errlen, "Global: checksum mismatch");
        goto bad;
    }
    if (!parse(g, err, errlen))
        goto bad;
    return true;
bad:
    fe_global_free(g);
    return false;
}

void fe_global_free(fe_global *g)
{
    free(g->data);
    g->data = NULL;
    g->size = 0;
}

bool fe_global_build(const fe_global *g, uint8_t **out, size_t *out_len, char *err, size_t errlen)
{
    *out = NULL;
    *out_len = 0;
    uint8_t *comp;
    size_t comp_len;
    huff_result hr = huff_compress(g->data, g->size, &comp, &comp_len);
    if (hr != HUFF_OK)
        return fail(err, errlen, "Global: compression failed: %s", huff_strerror(hr));

    size_t total = COMP_HDR + comp_len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        free(comp);
        return fail(err, errlen, "Global: out of memory");
    }
    memcpy(buf, "PMOC", 4);
    wr32(buf + 4, 2);
    wr32(buf + 8, g->size);
    wr32(buf + 12, fe_crc32(0, g->data, g->size));
    memcpy(buf + COMP_HDR, comp, comp_len);
    free(comp);

    // Verify: the finished file must load and match byte for byte
    fe_global v;
    char verr[96];
    bool ok = fe_global_load(&v, buf, total, verr, sizeof(verr));
    if (ok) {
        ok = v.size == g->size && memcmp(v.data, g->data, g->size) == 0;
        fe_global_free(&v);
    }
    if (!ok) {
        free(buf);
        return fail(err, errlen, "Global: rebuilt file failed verification");
    }
    *out = buf;
    *out_len = total;
    return true;
}

bool fe_global_game_clear(const fe_global *g)
{
    return g->data[g->flags_off] & 0x02;
}

void fe_global_set_game_clear(fe_global *g, bool on)
{
    if (on) g->data[g->flags_off] |= 0x02;
    else g->data[g->flags_off] &= (uint8_t)~0x02;
}

static int count_bits(const uint8_t *p, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        c += (p[i / 8] >> (i % 8)) & 1;
    return c;
}

static int set_bits(uint8_t *p, int n)
{
    int added = 0;
    for (int i = 0; i < n; i++) {
        if (!((p[i / 8] >> (i % 8)) & 1)) {
            p[i / 8] |= (uint8_t)(1 << (i % 8));
            added++;
        }
    }
    return added;
}

int fe_global_support_count(const fe_global *g)
{
    return count_bits(g->data + g->support_off, FE_SUPPORT_LOG_ENTRIES);
}

int fe_global_gallery_count(const fe_global *g)
{
    return count_bits(g->data + g->gallery_off, FE_GALLERY_ENTRIES);
}

int fe_global_complete_logs(fe_global *g)
{
    int added = set_bits(g->data + g->gallery_off, FE_GALLERY_ENTRIES);
    added += set_bits(g->data + g->colors_off, FE_HAIR_FLAG_ENTRIES);
    added += set_bits(g->data + g->support_off, FE_SUPPORT_LOG_ENTRIES);
    return added;
}

bool fe_global_is_complete(const fe_global *g)
{
    return fe_global_support_count(g) == FE_SUPPORT_LOG_ENTRIES && fe_global_gallery_count(g) == FE_GALLERY_ENTRIES;
}

bool fe_global_load_raw(fe_global *g, const uint8_t *data, size_t len, char *err, size_t errlen)
{
    memset(g, 0, sizeof(*g));
    if (len < 0x50 || len > 1024 * 1024)
        return fail(err, errlen, "Global snapshot has an implausible size");
    g->data = malloc(len);
    if (!g->data)
        return fail(err, errlen, "out of memory");
    memcpy(g->data, data, len);
    g->size = (uint32_t)len;
    if (!parse(g, err, errlen)) {
        fe_global_free(g);
        return false;
    }
    return true;
}

bool fe_global_copy_logs(fe_global *dst, const fe_global *src, char *err, size_t errlen)
{
    if (dst->gallery_bits != src->gallery_bits || dst->support_bits != src->support_bits ||
        dst->colors_bits != src->colors_bits)
        return fail(err, errlen, "the two Global files have different list sizes");
    memcpy(dst->data + dst->gallery_off, src->data + src->gallery_off, src->gallery_bits / 8 + 1);
    memcpy(dst->data + dst->support_off, src->data + src->support_off, src->support_bits / 8 + 1);
    memcpy(dst->data + dst->colors_off, src->data + src->colors_off, src->colors_bits / 8 + 1);
    fe_global_set_game_clear(dst, fe_global_game_clear(src));
    return true;
}

static uint8_t *avatar_profile(const fe_global *g, bool female)
{
    return g->data + g->avatar_off + (female ? 1 + FE_GLOBAL_AVATAR_SIZE : 0) + 1;
}

void fe_global_avatar_name(const fe_global *g, bool female, char *out, size_t outlen)
{
    const uint8_t *p = avatar_profile(g, female);
    size_t o = 0;
    for (int i = 0; i + 1 < 0x1A && o + 1 < outlen; i += 2) {
        uint16_t c = (uint16_t)(p[i] | (p[i + 1] << 8));
        if (!c) break;
        out[o++] = c < 0x80 ? (char)c : '?';
    }
    out[o] = '\0';
}

bool fe_global_avatar_empty(const fe_global *g, bool female)
{
    const uint8_t *p = avatar_profile(g, female);
    return p[0] == 0 && p[1] == 0;  // no name
}

void fe_global_set_avatar(fe_global *g, bool female, const uint8_t *logbook)
{
    memcpy(avatar_profile(g, female), logbook, FE_GLOBAL_AVATAR_SIZE);
}
