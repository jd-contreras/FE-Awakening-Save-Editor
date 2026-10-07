#pragma once

#include <stddef.h>
#include <stdint.h>

// Nintendo (GBA/DS BIOS style) 8-bit Huffman, as used inside Awakening's COMP blocks.
// Stream layout: u32 header (0x28 | decompressed_len << 8), tree, then 32-bit LE
// words read MSB first.

typedef enum {
    HUFF_OK = 0,
    HUFF_ERR_HEADER,     // not an 8-bit Huffman stream
    HUFF_ERR_LENGTH,     // header length does not match the expected size
    HUFF_ERR_TRUNCATED,  // ran out of input before producing all output
    HUFF_ERR_TREE,       // tree points outside the tree area
    HUFF_ERR_NOMEM,
    HUFF_ERR_INTERNAL,   // encoder produced an invalid tree (should never happen)
} huff_result;

const char *huff_strerror(huff_result r);

// Reads the decompressed length from the stream header. Returns HUFF_ERR_HEADER on a bad header.
huff_result huff_peek_length(const uint8_t *in, size_t in_len, uint32_t *out_len);

// Decompresses into out, which must be exactly out_len bytes (the header length).
huff_result huff_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

// Compresses in into a newly malloc'd buffer (*out, caller frees).
huff_result huff_compress(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
