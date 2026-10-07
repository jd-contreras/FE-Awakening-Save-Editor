// Importing Avatars from other saves, StreetPass/SpotPass teams and the Global logbook.
//
// Every imported unit becomes an Avatar character (id 0 male / 1 female, from its look), so
// it can build supports like (M)/(F) Robin. Its name, look, class, level, stats, skills,
// weapon ranks and items come along; supports, deployment and parent data do not.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/fesave.h"

#define FE_IMPORT_DATA_MAX 0x400

typedef enum {
    FE_IMPORT_UNIT,  // a full unit record from a save (forged items already turned into base weapons)
    FE_IMPORT_DU,    // a team/logbook unit: 0xF7 bytes of unit data + 0x187 logbook bytes
} fe_import_kind;

typedef struct {
    fe_import_kind kind;
    char name[32];
    char source[64];
    uint16_t len;
    uint8_t data[FE_IMPORT_DATA_MAX];
} fe_import_cand;

// Avatars (units with logbook data) in a save: the army, deployed and fallen units (unless
// skip_army) and the StreetPass/SpotPass teams on the world map. `prefix` starts each
// candidate's source text (e.g. "Slot 3"). Returns how many were added to out (up to max).
int fe_import_gather_save(const fe_save *s, const char *prefix, bool skip_army, fe_import_cand *out, int max);

// Logbook units in the decompressed Global file.
int fe_import_gather_logbook(const uint8_t *global, size_t size, fe_import_cand *out, int max);

// The 138 SpotPass/DLC heroes from the editor's data (Ike, Lyn, Marth...), as team units.
int fe_import_gather_heroes(fe_import_cand *out, int max);

// supports = true: the unit joins as (M)/(F) Robin (by its gender) and can build supports.
// supports = false: it stays a logbook unit (character 2, or its own id like Marth), no supports.
// Supports come along for Avatars from a save (A at most: S becomes A). If dst already has
// a unit of the same Avatar character, partners keep their side (it is shared by character),
// so only the imported unit's side is written.
// Adds the candidate to the end of dst's army. Needs one unit with Avatar look data in dst to
// use as a template. Returns the new unit's index, or -1 (nothing changed).
int fe_import_add(fe_save *dst, const fe_import_cand *c, bool supports, char *err, size_t errlen);
