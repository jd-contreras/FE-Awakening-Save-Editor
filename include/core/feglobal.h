#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The "Global" file shared by all slots: COMP block at offset 0 (no 0xC0 header),
// CRC32 over the decompressed data. Decompressed: INDE index (u32 offsets), then the
// USER block ("RESU"):
//   +0x04 13 bytes of flags/settings; +0x05 bit 1 (0x02) = "Game Clear" (shows the
//         Support Log, Theater and Unit Gallery menus)
//   then three bit lists, each "u32 count, count/8+1 bytes": Unit Gallery,
//   Support Log, hair-color flags (a gallery unit only shows if its color flag is set).
// Real file: gallery 64 bits, support log 1920 bits, colors 64 bits. FireEditor fills
// 53 gallery entries, 1830 support conversations and 52 colors; the editor does the same.

#define FE_GALLERY_ENTRIES 53
#define FE_SUPPORT_LOG_ENTRIES 1830
#define FE_HAIR_FLAG_ENTRIES 52

typedef struct {
    uint8_t *data;   // decompressed Global (owned)
    uint32_t size;
    uint32_t flags_off;     // byte holding the Game Clear bit
    uint32_t gallery_off, gallery_bits;
    uint32_t support_off, support_bits;
    uint32_t colors_off, colors_bits;
    uint32_t avatar_off;  // [06][male logbook 0x187][06][female logbook 0x187]
} fe_global;

bool fe_global_load(fe_global *g, const uint8_t *file, size_t len, char *err, size_t errlen);
void fe_global_free(fe_global *g);
// Recompresses and verifies (decode again + reload) like fe_save_build.
bool fe_global_build(const fe_global *g, uint8_t **out, size_t *out_len, char *err, size_t errlen);

// Support-scene Avatar profiles: after the flag lists come two logbook copies (each a 0x06
// byte + the first 0x187 bytes of a unit logbook block), male then female. The
// profiles' role in scenes is unclear: which Avatar conversations play is decided by the main
// Avatar's logbook gender byte, not by these (hardware test 2026-10-02, SAVE_FORMAT.md 15).
#define FE_GLOBAL_AVATAR_SIZE 0x187
void fe_global_avatar_name(const fe_global *g, bool female, char *out, size_t outlen);  // "" if empty
bool fe_global_avatar_empty(const fe_global *g, bool female);
// Copies a unit's logbook block (at least FE_GLOBAL_AVATAR_SIZE bytes) into the profile.
void fe_global_set_avatar(fe_global *g, bool female, const uint8_t *logbook);

bool fe_global_game_clear(const fe_global *g);
void fe_global_set_game_clear(fe_global *g, bool on);
int fe_global_support_count(const fe_global *g);   // set bits among the 1830 entries
int fe_global_gallery_count(const fe_global *g);   // set bits among the 53 entries
// Marks every Support Log conversation and Unit Gallery entry (plus hair-color flags) as seen.
// Returns how many bits were newly set.
int fe_global_complete_logs(fe_global *g);
// True when every Support Log and Unit Gallery entry is set.
bool fe_global_is_complete(const fe_global *g);

// Loads an already-decompressed Global (an undo snapshot). The data is copied.
bool fe_global_load_raw(fe_global *g, const uint8_t *data, size_t len, char *err, size_t errlen);
// Copies the Support Log, Unit Gallery, hair-color flags and the Game Clear flag from src
// into dst (both must have the same list sizes). Nothing else in dst changes.
bool fe_global_copy_logs(fe_global *dst, const fe_global *src, char *err, size_t errlen);
