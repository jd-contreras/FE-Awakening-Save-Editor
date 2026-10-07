#include "fs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include "log.h"

#define MAX_SAVE_FILE   (1024 * 1024)
#define BACKUP_ROOT     APP_DIR "/backups"

// Known Awakening application title IDs, used if the product code lookup fails
static const u64 KNOWN_TITLE_IDS[] = {
    0x00040000000A0500ULL,  // USA
    0x000400000009F100ULL,  // EUR
};

static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    LOGE("%s", err);
    return false;
}

static FS_Path utf16_path(const char *ascii, u16 *buf, size_t cap)
{
    size_t i = 0;
    for (; ascii[i] && i + 1 < cap; i++)
        buf[i] = (u8)ascii[i];
    buf[i] = 0;
    return (FS_Path){PATH_UTF16, (u32)((i + 1) * 2), buf};
}

static FS_Path file_path(const char *name, u16 *buf, size_t cap)
{
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "/%.31s", name);
    return utf16_path(tmp, buf, cap);
}

const char *fs_media_name(FS_MediaType m)
{
    return m == MEDIATYPE_GAME_CARD ? "Cartridge" : m == MEDIATYPE_SD ? "SD (eShop)" : "NAND";
}

//---------------------------------------------------------------------------
// Title discovery
//---------------------------------------------------------------------------
static bool is_awakening(FS_MediaType media, u64 tid, char product[16])
{
    // Only base applications; updates (0004000E) and DLC (0004008C) share the product code
    if ((tid >> 32) != 0x00040000)
        return false;

    memset(product, 0, 16);
    Result rc = AM_GetTitleProductCode(media, tid, product);
    if (R_SUCCEEDED(rc) && strlen(product) >= 9 && strncmp(product, "CTR-", 4) == 0 &&
        strncmp(product + 6, "AFE", 3) == 0)
        return true;

    for (size_t i = 0; i < sizeof(KNOWN_TITLE_IDS) / sizeof(KNOWN_TITLE_IDS[0]); i++) {
        if (tid == KNOWN_TITLE_IDS[i]) {
            if (!product[0])
                snprintf(product, 16, "(known ID)");
            return true;
        }
    }
    return false;
}

static int scan_media(FS_MediaType media, fe_title *out, int max)
{
    u32 count = 0;
    Result rc = AM_GetTitleCount(media, &count);
    if (R_FAILED(rc)) {
        // No cartridge inserted lands here; that's normal
        LOGI("%s: title count unavailable (rc=0x%08lX)", fs_media_name(media), (unsigned long)rc);
        return 0;
    }
    LOGI("%s: %lu titles installed", fs_media_name(media), (unsigned long)count);
    if (count == 0)
        return 0;

    u64 *ids = malloc(sizeof(u64) * count);
    if (!ids)
        return 0;
    u32 read = 0;
    rc = AM_GetTitleList(&read, media, count, ids);
    if (R_FAILED(LOG_RC("AM_GetTitleList", rc))) {
        free(ids);
        return 0;
    }

    int found = 0;
    for (u32 i = 0; i < read && found < max; i++) {
        char product[16];
        if (is_awakening(media, ids[i], product)) {
            out[found].title_id = ids[i];
            out[found].media = media;
            memcpy(out[found].product, product, sizeof(product));
            LOGI("found Awakening: %016llX %s on %s", (unsigned long long)ids[i], product, fs_media_name(media));
            found++;
        }
    }
    free(ids);
    return found;
}

int fs_find_titles(fe_title *out, int max)
{
    int n = scan_media(MEDIATYPE_SD, out, max);
    n += scan_media(MEDIATYPE_GAME_CARD, out + n, max - n);
    return n;
}

//---------------------------------------------------------------------------
// Archive access
//---------------------------------------------------------------------------
Result fs_open_save(const fe_title *t, FS_Archive *arch)
{
    u32 path[3] = {t->media, (u32)(t->title_id & 0xFFFFFFFF), (u32)(t->title_id >> 32)};
    FS_Path p = {PATH_BINARY, sizeof(path), path};
    Result rc = FSUSER_OpenArchive(arch, ARCHIVE_USER_SAVEDATA, p);
    LOG_RC("FSUSER_OpenArchive(USER_SAVEDATA)", rc);
    return rc;
}

void fs_close_save(FS_Archive arch)
{
    LOG_RC("FSUSER_CloseArchive", FSUSER_CloseArchive(arch));
}

bool fs_has_luma_mod(const fe_title *t, char *path_out, size_t path_len)
{
    struct stat st;
    snprintf(path_out, path_len, "sdmc:/luma/titles/%016llX", (unsigned long long)t->title_id);
    bool found = stat(path_out, &st) == 0 && S_ISDIR(st.st_mode);
    LOGI("game mod folder %s: %s", path_out, found ? "PRESENT" : "none");
    return found;
}

Result fs_list_root(FS_Archive arch, fs_entry *out, int max, int *count)
{
    *count = 0;
    u16 pbuf[4];
    Handle dir;
    Result rc = FSUSER_OpenDirectory(&dir, arch, utf16_path("/", pbuf, 4));
    if (R_FAILED(LOG_RC("FSUSER_OpenDirectory(/)", rc)))
        return rc;

    FS_DirectoryEntry entry;
    u32 read = 0;
    while (*count < max) {
        rc = FSDIR_Read(dir, &read, 1, &entry);
        if (R_FAILED(rc) || read == 0)
            break;
        if (entry.attributes & FS_ATTRIBUTE_DIRECTORY)
            continue;
        fs_entry *e = &out[*count];
        size_t i = 0;
        for (; entry.name[i] && i + 1 < sizeof(e->name); i++)
            e->name[i] = entry.name[i] < 0x80 ? (char)entry.name[i] : '?';
        e->name[i] = '\0';
        e->size = entry.fileSize;
        LOGI("  save file: %-10s %llu bytes", e->name, (unsigned long long)e->size);
        (*count)++;
    }
    FSDIR_Close(dir);
    return R_FAILED(rc) ? rc : 0;
}

bool fs_has_file(const fs_entry *entries, int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0)
            return true;
    return false;
}

Result fs_read_file(FS_Archive arch, const char *name, u8 **buf, u32 *size)
{
    *buf = NULL;
    *size = 0;
    u16 pbuf[40];
    Handle h;
    Result rc = FSUSER_OpenFile(&h, arch, file_path(name, pbuf, 40), FS_OPEN_READ, 0);
    if (R_FAILED(rc)) {
        LOGE("open %s for read failed: 0x%08lX", name, (unsigned long)rc);
        return rc;
    }

    u64 sz = 0;
    rc = FSFILE_GetSize(h, &sz);
    if (R_SUCCEEDED(rc) && (sz == 0 || sz > MAX_SAVE_FILE))
        rc = MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION, RD_INVALID_SIZE);

    u8 *data = NULL;
    if (R_SUCCEEDED(rc)) {
        data = malloc((size_t)sz);
        if (!data)
            rc = MAKERESULT(RL_FATAL, RS_OUTOFRESOURCE, RM_APPLICATION, RD_OUT_OF_MEMORY);
    }
    u32 got = 0;
    if (R_SUCCEEDED(rc))
        rc = FSFILE_Read(h, &got, 0, data, (u32)sz);
    FSFILE_Close(h);

    if (R_FAILED(rc) || got != sz) {
        LOGE("read %s failed: rc=0x%08lX, %lu of %llu bytes", name, (unsigned long)rc,
             (unsigned long)got, (unsigned long long)sz);
        free(data);
        return R_FAILED(rc) ? rc : MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_APPLICATION, RD_INVALID_SIZE);
    }
    *buf = data;
    *size = (u32)sz;
    return 0;
}

//---------------------------------------------------------------------------
// Backup
//---------------------------------------------------------------------------
static bool make_dir(const char *path)
{
    return mkdir(path, 0777) == 0 || errno == EEXIST;
}

static bool write_sd_file(const char *path, const u8 *data, u32 size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        return false;

    // Read back and compare
    f = fopen(path, "rb");
    if (!f)
        return false;
    u8 *check = malloc(size ? size : 1);
    ok = check && fread(check, 1, size, f) == size && memcmp(check, data, size) == 0;
    free(check);
    fclose(f);
    return ok;
}

bool fs_backup_save(FS_Archive arch, char *out_dir, size_t out_len, char *err, size_t errlen)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(out_dir, out_len, BACKUP_ROOT "/%04d-%02d-%02d_%02d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);

    if (!make_dir(APP_DIR) || !make_dir(BACKUP_ROOT) || !make_dir(out_dir))
        return fail(err, errlen, "Could not create backup folder\n%s\n(errno %d). Is the SD card full or locked?",
                    out_dir, errno);
    LOGI("backing up save to %s", out_dir);

    fs_entry entries[FS_MAX_ENTRIES];
    int count = 0;
    Result rc = fs_list_root(arch, entries, FS_MAX_ENTRIES, &count);
    if (R_FAILED(rc) || count == 0)
        return fail(err, errlen, "Could not list save files for backup (rc=0x%08lX)", (unsigned long)rc);

    for (int i = 0; i < count; i++) {
        u8 *data;
        u32 size;
        rc = fs_read_file(arch, entries[i].name, &data, &size);
        if (R_FAILED(rc))
            return fail(err, errlen, "Backup failed reading %s (rc=0x%08lX)", entries[i].name, (unsigned long)rc);

        char path[160];
        snprintf(path, sizeof(path), "%s/%s", out_dir, entries[i].name);
        bool ok = write_sd_file(path, data, size);
        free(data);
        if (!ok)
            return fail(err, errlen, "Backup failed writing/verifying\n%s", path);
        LOGI("  backed up %s (%lu bytes), verified", entries[i].name, (unsigned long)size);
    }
    LOGI("backup complete: %d files", count);
    return true;
}

//---------------------------------------------------------------------------
// Write back
//---------------------------------------------------------------------------
bool fs_write_and_commit(FS_Archive arch, const char *name, const u8 *buf, u32 size,
                         char *err, size_t errlen)
{
    u16 pbuf[40];
    FS_Path p = file_path(name, pbuf, 40);
    Handle h;
    u32 written = 0;

    LOGI("writing %s (%lu bytes)", name, (unsigned long)size);

    // Nothing below is permanent until the commit; if a step fails the archive
    // is closed without committing and the game keeps the old data.
    Result rc = FSUSER_DeleteFile(arch, p);
    if (R_FAILED(LOG_RC("FSUSER_DeleteFile", rc)))
        return fail(err, errlen, "Could not replace %s (delete rc=0x%08lX).\nNothing was committed.", name, (unsigned long)rc);

    rc = FSUSER_CreateFile(arch, p, 0, size);
    if (R_FAILED(LOG_RC("FSUSER_CreateFile", rc)))
        return fail(err, errlen, "Could not create %s (rc=0x%08lX).\nNothing was committed.", name, (unsigned long)rc);

    rc = FSUSER_OpenFile(&h, arch, p, FS_OPEN_WRITE, 0);
    if (R_FAILED(LOG_RC("FSUSER_OpenFile(write)", rc)))
        return fail(err, errlen, "Could not open %s for writing (rc=0x%08lX).\nNothing was committed.", name, (unsigned long)rc);

    rc = FSFILE_Write(h, &written, 0, buf, size, FS_WRITE_FLUSH);
    FSFILE_Close(h);
    if (R_FAILED(LOG_RC("FSFILE_Write", rc)) || written != size)
        return fail(err, errlen, "Write to %s failed (rc=0x%08lX, %lu/%lu bytes).\nNothing was committed.",
                    name, (unsigned long)rc, (unsigned long)written, (unsigned long)size);

    rc = FSUSER_ControlArchive(arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
    if (R_FAILED(LOG_RC("FSUSER_ControlArchive(COMMIT)", rc)))
        return fail(err, errlen, "Commit failed (rc=0x%08lX).\nThe save may be unchanged; restore the backup if the game misbehaves.",
                    (unsigned long)rc);

    // Read back what is now in the save and compare
    u8 *check;
    u32 check_size;
    rc = fs_read_file(arch, name, &check, &check_size);
    if (R_FAILED(rc))
        return fail(err, errlen, "Saved, but could not read %s back to verify (rc=0x%08lX)", name, (unsigned long)rc);
    bool same = check_size == size && memcmp(check, buf, size) == 0;
    free(check);
    if (!same)
        return fail(err, errlen, "Saved, but read-back of %s does not match!\nRestore the backup before playing.", name);

    LOGI("write of %s committed and verified", name);
    return true;
}

//---------------------------------------------------------------------------
// Backup management
//---------------------------------------------------------------------------
static int cmp_backup(const void *a, const void *b)
{
    return strcmp(((const fs_backup *)b)->name, ((const fs_backup *)a)->name);  // newest first
}

int fs_list_backups(fs_backup *out, int max)
{
    DIR *d = opendir(BACKUP_ROOT);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(out[n].name)) continue;
        char dir[96];
        snprintf(dir, sizeof(dir), BACKUP_ROOT "/%s", e->d_name);
        DIR *sub = opendir(dir);
        if (!sub) continue;
        fs_backup *b = &out[n];
        snprintf(b->name, sizeof(b->name), "%s", e->d_name);
        b->files = 0;
        b->bytes = 0;
        struct dirent *f;
        while ((f = readdir(sub))) {
            if (f->d_name[0] == '.') continue;
            char path[160];
            snprintf(path, sizeof(path), "%s/%.60s", dir, f->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                b->files++;
                b->bytes += (u32)st.st_size;
            }
        }
        closedir(sub);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(fs_backup), cmp_backup);
    return n;
}

static u8 *read_sd_file(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = sz > 0 && sz <= MAX_SAVE_FILE ? malloc((size_t)sz) : NULL;
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *size = (u32)sz;
    return buf;
}

bool fs_restore_backup(FS_Archive arch, const char *name, char *err, size_t errlen)
{
    char dir[96];
    snprintf(dir, sizeof(dir), BACKUP_ROOT "/%s", name);
    DIR *d = opendir(dir);
    if (!d) return fail(err, errlen, "Backup folder not found:\n%s", dir);

    struct { char name[32]; u8 *data; u32 size; } files[FS_MAX_ENTRIES];
    int n = 0;
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n >= FS_MAX_ENTRIES || strlen(e->d_name) >= 32) { ok = false; break; }
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        snprintf(files[n].name, sizeof(files[n].name), "%s", e->d_name);
        files[n].data = read_sd_file(path, &files[n].size);
        if (!files[n].data) { ok = false; n++; break; }
        n++;
    }
    closedir(d);
    if (!ok || n == 0) {
        for (int i = 0; i < n; i++) free(files[i].data);
        return fail(err, errlen, n ? "Could not read the backup files in\n%s" : "The backup folder is empty:\n%s", dir);
    }
    LOGI("restoring backup %s (%d files)", name, n);

    fs_entry cur[FS_MAX_ENTRIES];
    int cur_count = 0;
    Result rc = fs_list_root(arch, cur, FS_MAX_ENTRIES, &cur_count);
    if (R_FAILED(rc)) {
        for (int i = 0; i < n; i++) free(files[i].data);
        return fail(err, errlen, "Could not list the save files (rc=0x%08lX).", (unsigned long)rc);
    }

    // Write every file (nothing is permanent until the commit)
    for (int i = 0; i < n && ok; i++) {
        u16 pbuf[40];
        FS_Path p = file_path(files[i].name, pbuf, 40);
        FSUSER_DeleteFile(arch, p);  // may not exist
        Handle h;
        u32 written = 0;
        rc = FSUSER_CreateFile(arch, p, 0, files[i].size);
        if (R_SUCCEEDED(rc)) rc = FSUSER_OpenFile(&h, arch, p, FS_OPEN_WRITE, 0);
        if (R_SUCCEEDED(rc)) {
            rc = FSFILE_Write(h, &written, 0, files[i].data, files[i].size, FS_WRITE_FLUSH);
            FSFILE_Close(h);
        }
        if (R_FAILED(LOG_RC("restore write", rc)) || written != files[i].size) {
            fail(err, errlen, "Could not write %s (rc=0x%08lX).\nNothing was committed.", files[i].name, (unsigned long)rc);
            ok = false;
        }
    }
    // Remove save files the backup doesn't have (e.g. a slot started later)
    for (int i = 0; i < cur_count && ok; i++) {
        bool keep = false;
        for (int k = 0; k < n; k++) keep |= strcmp(cur[i].name, files[k].name) == 0;
        if (keep) continue;
        u16 pbuf[40];
        rc = FSUSER_DeleteFile(arch, file_path(cur[i].name, pbuf, 40));
        LOGI("restore: removing %s (not in the backup) rc=0x%08lX", cur[i].name, (unsigned long)rc);
    }
    if (ok) {
        rc = FSUSER_ControlArchive(arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        if (R_FAILED(LOG_RC("FSUSER_ControlArchive(COMMIT)", rc))) {
            fail(err, errlen, "Commit failed (rc=0x%08lX).", (unsigned long)rc);
            ok = false;
        }
    }
    for (int i = 0; i < n && ok; i++) {  // read back
        u8 *check;
        u32 size;
        rc = fs_read_file(arch, files[i].name, &check, &size);
        bool same = R_SUCCEEDED(rc) && size == files[i].size && memcmp(check, files[i].data, size) == 0;
        if (R_SUCCEEDED(rc)) free(check);
        if (!same) {
            fail(err, errlen, "Restored, but %s does not read back the same.", files[i].name);
            ok = false;
        }
    }
    for (int i = 0; i < n; i++) free(files[i].data);
    if (ok) LOGI("restore of %s committed and verified", name);
    return ok;
}

bool fs_delete_backup(const char *name)
{
    char dir[96];
    snprintf(dir, sizeof(dir), BACKUP_ROOT "/%s", name);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    char paths[FS_MAX_ENTRIES * 2][160];
    int n = 0;
    while ((e = readdir(d)) && n < FS_MAX_ENTRIES * 2) {
        if (e->d_name[0] == '.') continue;
        snprintf(paths[n++], 160, "%s/%.60s", dir, e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) unlink(paths[i]);
    bool ok = rmdir(dir) == 0;
    LOGI("deleted backup %s: %s", name, ok ? "ok" : "failed");
    return ok;
}

int fs_delete_all_backups(void)
{
    fs_backup *list = malloc(sizeof(fs_backup) * FS_MAX_BACKUPS);
    if (!list) return 0;
    int n = fs_list_backups(list, FS_MAX_BACKUPS), done = 0;
    for (int i = 0; i < n; i++) done += fs_delete_backup(list[i].name);
    free(list);
    return done;
}
