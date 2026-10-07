#pragma once

#include <stddef.h>
#include <stdint.h>

// Standard IEEE CRC32 (zlib-compatible). Start with crc = 0; chain calls by
// passing the previous result.
uint32_t fe_crc32(uint32_t crc, const uint8_t *data, size_t len);
