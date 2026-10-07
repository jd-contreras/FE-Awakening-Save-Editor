// 8-bit Huffman codec for Fire Emblem Awakening saves.
//
// The decoder is written from the GBATEK description of the BIOS Huffman format,
// with bounds checks on every read.
// The encoder's tree layout (the part that keeps every child offset within 6 bits)
// is ported from CUE's huffman.c (GPLv3) via SciresM's FEAST, which is the
// encoder the community has used on Awakening saves for years.

#include "core/huffman.h"

#include <stdlib.h>
#include <string.h>

#define CMD_CODE    0x28
#define MAX_SYMBOLS 256
#define HUF_LCHAR   0x80
#define HUF_RCHAR   0x40
#define HUF_NEXT    0x3F
#define MAX_CODE_BITS 256

const char *huff_strerror(huff_result r)
{
    switch (r) {
    case HUFF_OK:            return "ok";
    case HUFF_ERR_HEADER:    return "not an 8-bit Huffman stream";
    case HUFF_ERR_LENGTH:    return "unexpected decompressed length";
    case HUFF_ERR_TRUNCATED: return "compressed data is truncated";
    case HUFF_ERR_TREE:      return "corrupt Huffman tree";
    case HUFF_ERR_NOMEM:     return "out of memory";
    case HUFF_ERR_INTERNAL:  return "internal encoder error";
    }
    return "unknown error";
}

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

huff_result huff_peek_length(const uint8_t *in, size_t in_len, uint32_t *out_len)
{
    if (in_len < 8)
        return HUFF_ERR_HEADER;
    uint32_t hdr = rd32(in);
    if ((hdr & 0xFF) != CMD_CODE)
        return HUFF_ERR_HEADER;
    *out_len = hdr >> 8;
    return HUFF_OK;
}

//---------------------------------------------------------------------------
// Decoder
//---------------------------------------------------------------------------
huff_result huff_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    uint32_t declared;
    huff_result r = huff_peek_length(in, in_len, &declared);
    if (r != HUFF_OK)
        return r;
    if (declared != out_len)
        return HUFF_ERR_LENGTH;

    const size_t tree_end = 4 + (((size_t)in[4] + 1) << 1);
    if (tree_end + 4 > in_len)
        return HUFF_ERR_TRUNCATED;

    const size_t root = 5;
    size_t node_off = root;
    uint8_t node = in[root];
    size_t pos = tree_end;
    size_t outpos = 0;

    while (outpos < out_len) {
        if (pos + 4 > in_len)
            return HUFF_ERR_TRUNCATED;
        uint32_t word = rd32(in + pos);
        pos += 4;

        for (int b = 31; b >= 0 && outpos < out_len; b--) {
            unsigned bit = (word >> b) & 1;
            size_t child = (node_off & ~(size_t)1) + ((size_t)(node & HUF_NEXT) << 1) + 2 + bit;
            if (child >= tree_end)
                return HUFF_ERR_TREE;

            if (node & (bit ? HUF_RCHAR : HUF_LCHAR)) {
                out[outpos++] = in[child];
                node_off = root;
                node = in[root];
            } else {
                node_off = child;
                node = in[child];
            }
        }
    }
    return HUFF_OK;
}

//---------------------------------------------------------------------------
// Encoder
//---------------------------------------------------------------------------
typedef struct {
    uint32_t symbol;
    uint32_t weight;
    uint32_t leafs;
    int dad, lson, rson;  // indices into enc.tree, -1 = none
} hnode;

typedef struct {
    uint32_t nbits;
    uint8_t work[MAX_CODE_BITS / 8];
} hcode;

typedef struct {
    uint32_t freqs[MAX_SYMBOLS];
    hnode *tree;
    uint32_t num_leafs, num_nodes;
    uint8_t *codetree, *codemask;
    uint32_t max_nodes;
    hcode codes[MAX_SYMBOLS];
    int failed;
} henc;

static void create_freqs(henc *e, const uint8_t *data, size_t len)
{
    memset(e->freqs, 0, sizeof(e->freqs));
    for (size_t i = 0; i < len; i++)
        e->freqs[data[i]]++;

    uint32_t leafs = 0;
    for (int i = 0; i < MAX_SYMBOLS; i++)
        if (e->freqs[i]) leafs++;

    // A Huffman tree needs at least two leaves; pad with dummy symbols.
    // (CUE's original miscounts here; recounting avoids that.)
    if (leafs < 2) {
        for (int i = 0; i < MAX_SYMBOLS && leafs < 2; i++) {
            if (!e->freqs[i]) {
                e->freqs[i] = 1;
                leafs++;
            }
        }
    }
    e->num_leafs = leafs;
    e->num_nodes = (leafs << 1) - 1;
}

static int create_tree(henc *e)
{
    e->tree = calloc(e->num_nodes, sizeof(hnode));
    if (!e->tree)
        return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_SYMBOLS; i++) {
        if (e->freqs[i]) {
            hnode *node = &e->tree[n++];
            node->symbol = i;
            node->weight = e->freqs[i];
            node->leafs = 1;
            node->dad = node->lson = node->rson = -1;
        }
    }

    while (n < e->num_nodes) {
        int lnode = -1, rnode = -1;
        uint32_t lweight = 0, rweight = 0;

        for (uint32_t i = 0; i < n; i++) {
            if (e->tree[i].dad != -1)
                continue;
            uint32_t w = e->tree[i].weight;
            if (lweight == 0 || w < lweight) {
                rweight = lweight;
                rnode = lnode;
                lweight = w;
                lnode = (int)i;
            } else if (rweight == 0 || w < rweight) {
                rweight = w;
                rnode = (int)i;
            }
        }
        if (lnode < 0 || rnode < 0)
            return 0;

        hnode *node = &e->tree[n];
        node->symbol = n - e->num_leafs + MAX_SYMBOLS;
        node->weight = e->tree[lnode].weight + e->tree[rnode].weight;
        node->leafs = e->tree[lnode].leafs + e->tree[rnode].leafs;
        node->dad = -1;
        node->lson = lnode;
        node->rson = rnode;
        e->tree[lnode].dad = e->tree[rnode].dad = (int)n;
        n++;
    }
    return 1;
}

static uint32_t create_code_branch(henc *e, int root, uint32_t p, uint32_t q)
{
    hnode *t = e->tree;
    uint32_t mask;

    if (q + 2 > e->max_nodes || p >= e->max_nodes) {
        e->failed = 1;
        return t[root].leafs;
    }

    if (t[root].leafs <= HUF_NEXT + 1) {
        // Small subtree: breadth-first layout always fits in 6-bit offsets.
        int *stack = malloc(sizeof(int) * 2 * t[root].leafs);
        if (!stack) {
            e->failed = 1;
            return t[root].leafs;
        }
        uint32_t s = 0, r = 0;
        stack[r++] = root;

        while (s < r) {
            hnode *node = &t[stack[s++]];
            uint32_t slot = (s == 1) ? p : q++;
            if (slot >= e->max_nodes) {
                e->failed = 1;
                break;
            }
            if (node->leafs == 1) {
                e->codetree[slot] = (uint8_t)node->symbol;
                e->codemask[slot] = 0xFF;
            } else {
                mask = 0;
                if (t[node->lson].leafs == 1) mask |= HUF_LCHAR;
                if (t[node->rson].leafs == 1) mask |= HUF_RCHAR;
                e->codetree[slot] = (uint8_t)((r - s) >> 1);
                e->codemask[slot] = (uint8_t)mask;
                stack[r++] = node->lson;
                stack[r++] = node->rson;
            }
        }
        free(stack);
    } else {
        hnode *node = &t[root];
        mask = 0;
        if (t[node->lson].leafs == 1) mask |= HUF_LCHAR;
        if (t[node->rson].leafs == 1) mask |= HUF_RCHAR;

        e->codetree[p] = 0;
        e->codemask[p] = (uint8_t)mask;

        if (t[node->lson].leafs <= t[node->rson].leafs) {
            uint32_t l_leafs = create_code_branch(e, node->lson, q, q + 2);
            create_code_branch(e, node->rson, q + 1, q + (l_leafs << 1));
            e->codetree[q + 1] = (uint8_t)(l_leafs - 1);
        } else {
            uint32_t r_leafs = create_code_branch(e, node->rson, q + 1, q + 2);
            create_code_branch(e, node->lson, q, q + (r_leafs << 1));
            e->codetree[q] = (uint8_t)(r_leafs - 1);
        }
    }
    return t[root].leafs;
}

// Moves subtrees around until every child offset fits in 6 bits.
static void update_code_tree(henc *e)
{
    uint8_t *ct = e->codetree, *cm = e->codemask;
    int max = (ct[0] + 1) << 1;

    for (int i = 1; i < max; i++) {
        if (cm[i] == 0xFF || ct[i] <= HUF_NEXT)
            continue;

        int inc;
        if ((i & 1) && ct[i - 1] == HUF_NEXT) {
            i--;
            inc = 1;
        } else if (!(i & 1) && ct[i + 1] == HUF_NEXT) {
            i++;
            inc = 1;
        } else {
            inc = ct[i] - HUF_NEXT;
        }

        int n1 = (i >> 1) + 1 + ct[i];
        int n0 = n1 - inc;
        int l1 = n1 << 1;
        int l0 = n0 << 1;
        if (l1 + 1 >= (int)e->max_nodes || l0 < 0) {
            e->failed = 1;
            return;
        }

        uint8_t t0 = ct[l1], t1 = ct[l1 + 1];
        uint8_t m0 = cm[l1], m1 = cm[l1 + 1];
        for (int j = l1; j > l0; j -= 2) {
            ct[j] = ct[j - 2];
            ct[j + 1] = ct[j - 1];
            cm[j] = cm[j - 2];
            cm[j + 1] = cm[j - 1];
        }
        ct[l0] = t0;
        ct[l0 + 1] = t1;
        cm[l0] = m0;
        cm[l0 + 1] = m1;

        ct[i] = (uint8_t)(ct[i] - inc);

        for (int j = i + 1; j < l0; j++) {
            if (cm[j] != 0xFF) {
                int k = (j >> 1) + 1 + ct[j];
                if (k >= n0 && k < n1) ct[j]++;
            }
        }

        if (cm[l0 + 0] != 0xFF) ct[l0 + 0] = (uint8_t)(ct[l0 + 0] + inc);
        if (cm[l0 + 1] != 0xFF) ct[l0 + 1] = (uint8_t)(ct[l0 + 1] + inc);

        for (int j = l0 + 2; j < l1 + 2; j++) {
            if (cm[j] != 0xFF) {
                int k = (j >> 1) + 1 + ct[j];
                if (k > n1) ct[j]--;
            }
        }

        i = (i | 1) - 2;
        if (i < 0)
            i = 0;  // loop increment brings it back to 1; slot 0 is the size byte
    }
}

static int create_code_tree(henc *e)
{
    // A few spare bytes so the pair shuffling can never index past the end.
    e->max_nodes = ((((e->num_leafs - 1) | 1) + 1) << 1);
    e->codetree = calloc(e->max_nodes + 4, 1);
    e->codemask = calloc(e->max_nodes + 4, 1);
    if (!e->codetree || !e->codemask)
        return 0;

    e->codetree[0] = (uint8_t)((e->num_leafs - 1) | 1);
    e->codemask[0] = 0;

    create_code_branch(e, (int)e->num_nodes - 1, 1, 2);
    if (e->failed)
        return 0;
    update_code_tree(e);
    if (e->failed)
        return 0;

    for (int i = ((e->codetree[0] + 1) << 1) - 1; i > 0; i--)
        if (e->codemask[i] != 0xFF)
            e->codetree[i] |= e->codemask[i];
    return 1;
}

static int create_code_works(henc *e)
{
    uint8_t path[MAX_CODE_BITS];

    memset(e->codes, 0, sizeof(e->codes));
    for (uint32_t i = 0; i < e->num_leafs; i++) {
        int node = (int)i;
        uint32_t symbol = e->tree[i].symbol;
        uint32_t nbits = 0;

        while (e->tree[node].dad != -1) {
            if (nbits >= MAX_CODE_BITS)
                return 0;
            int dad = e->tree[node].dad;
            path[nbits++] = (e->tree[dad].lson == node) ? 0 : 1;
            node = dad;
        }

        hcode *code = &e->codes[symbol];
        code->nbits = nbits;
        // path[] is leaf->root; the bitstream wants root->leaf, MSB first.
        for (uint32_t b = 0; b < nbits; b++)
            if (path[nbits - 1 - b])
                code->work[b >> 3] |= (uint8_t)(0x80 >> (b & 7));
    }
    return 1;
}

static void henc_free(henc *e)
{
    free(e->tree);
    free(e->codetree);
    free(e->codemask);
}

huff_result huff_compress(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (in_len > 0xFFFFFF)
        return HUFF_ERR_LENGTH;

    henc *e = calloc(1, sizeof(henc));
    if (!e)
        return HUFF_ERR_NOMEM;

    huff_result result = HUFF_ERR_INTERNAL;
    create_freqs(e, in, in_len);
    if (!create_tree(e) || !create_code_tree(e) || !create_code_works(e))
        goto done;

    // Size the output exactly: header + tree + bitstream padded to 32-bit words.
    uint64_t total_bits = 0;
    for (size_t i = 0; i < in_len; i++)
        total_bits += e->codes[in[i]].nbits;
    size_t tree_len = ((size_t)e->codetree[0] + 1) << 1;
    size_t size = 4 + tree_len + (size_t)((total_bits + 31) / 32) * 4;

    uint8_t *buf = calloc(size, 1);
    if (!buf) {
        result = HUFF_ERR_NOMEM;
        goto done;
    }

    wr32(buf, CMD_CODE | ((uint32_t)in_len << 8));
    memcpy(buf + 4, e->codetree, tree_len);

    size_t pos = 4 + tree_len;
    uint32_t word = 0;
    int bitpos = 31;
    for (size_t i = 0; i < in_len; i++) {
        const hcode *code = &e->codes[in[i]];
        for (uint32_t b = 0; b < code->nbits; b++) {
            if (code->work[b >> 3] & (0x80 >> (b & 7)))
                word |= 1u << bitpos;
            if (--bitpos < 0) {
                wr32(buf + pos, word);
                pos += 4;
                word = 0;
                bitpos = 31;
            }
        }
    }
    if (bitpos != 31) {
        wr32(buf + pos, word);
        pos += 4;
    }

    *out = buf;
    *out_len = pos;
    result = HUFF_OK;

done:
    henc_free(e);
    free(e);
    return result;
}
