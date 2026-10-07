#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Save-archive access through the system FS service (same approach as Checkpoint).

#define FS_MAX_TITLES  8
#define FS_MAX_ENTRIES 16

typedef struct {
    u64 title_id;
    FS_MediaType media;
    char product[16];   // e.g. "CTR-P-AFEE"
} fe_title;

typedef struct {
    char name[32];
    u64 size;
} fs_entry;

// Scans SD and cartridge for Fire Emblem Awakening (product code CTR-?-AFE?,
// or a known title ID). Returns how many were found.
int fs_find_titles(fe_title *out, int max);
const char *fs_media_name(FS_MediaType m);

Result fs_open_save(const fe_title *t, FS_Archive *arch);
void fs_close_save(FS_Archive arch);

// Lists files in the root of the save archive.
Result fs_list_root(FS_Archive arch, fs_entry *out, int max, int *count);
bool fs_has_file(const fs_entry *entries, int count, const char *name);

// True if Luma3DS has a game mod folder for this title (sdmc:/luma/titles/<TID>/).
// Mods there (LayeredFS romfs, code patches) can change base stats, so the
// in-game numbers may differ from the vanilla tables the editor uses.
bool fs_has_luma_mod(const fe_title *t, char *path_out, size_t path_len);

// Reads a whole file into a malloc'd buffer.
Result fs_read_file(FS_Archive arch, const char *name, u8 **buf, u32 *size);

// Copies every file in the save root to sdmc:/3ds/FEAEditor/backups/<timestamp>/
// and reads each copy back to verify it. out_dir receives the folder path.
bool fs_backup_save(FS_Archive arch, char *out_dir, size_t out_len, char *err, size_t errlen);

// Replaces one file in the save and commits the archive, then reads it back
// and compares. Uses the delete/create/write/commit sequence Checkpoint uses.
// Backups made by the editor (sdmc:/3ds/FEAEditor/backups/<date_time>/), newest first.
#define FS_MAX_BACKUPS 64
typedef struct {
    char name[32];
    int files;
    u32 bytes;
} fs_backup;
int fs_list_backups(fs_backup *out, int max);
// Makes the save exactly the backup: writes every file in it, deletes save files it doesn't
// have, then commits once and reads everything back. Nothing is committed if a step fails.
bool fs_restore_backup(FS_Archive arch, const char *name, char *err, size_t errlen);
bool fs_delete_backup(const char *name);
int fs_delete_all_backups(void);  // returns how many were deleted

bool fs_write_and_commit(FS_Archive arch, const char *name, const u8 *buf, u32 size,
                         char *err, size_t errlen);
