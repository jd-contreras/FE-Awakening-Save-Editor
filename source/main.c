// Awakening Save Editor - entry point and screen flow.
//
// Flow: find the game -> pick a slot -> unit list -> edit level/stats (or gold)
//       -> START: confirm -> backup whole save to SD -> rebuild + verify -> write + commit + read back.

#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include "core/fesave.h"
#include "core/femod.h"
#include "core/feglobal.h"
#include "core/feimport.h"
#include "fs.h"
#include "log.h"
#include "ui.h"

#define SLOT_COUNT   3
#define LIST_ROWS    11
#define ROW_H        17.0f
#define EDIT_ROWS    (1 + FE_STAT_COUNT)  // level + 8 stats

typedef enum {
    ST_FATAL,
    ST_PICK_TITLE,
    ST_PICK_SLOT,
    ST_UNITS,
    ST_EDIT,
    ST_SKILLS,
    ST_CLASS,
    ST_SUPPORTS,
    ST_ARMY,
    ST_ITEMS,
    ST_ITEM_PICK,
    ST_CONVOY,
    ST_EXTRAS,
    ST_AVATAR,
    ST_LOOK_PICK,
    ST_GLOBAL,
    ST_GLOBAL_RESTORE,
    ST_ADD_UNIT,
    ST_MORE,
    ST_ARMY_TOOLS,
    ST_WORLD_MAP,
    ST_IMPORT_MENU,
    ST_IMPORT_PICK,
    ST_PARENTS,
    ST_BACKUPS,
    ST_FORGE,
    ST_RECORDS,
    ST_RECORD_EDIT,
    ST_RECORD_PICK,
    ST_BARRACKS,
    ST_CONFIRM_SAVE,
    ST_CONFIRM_DISCARD,
    ST_MESSAGE,
} state_t;

typedef struct {
    bool present;
    bool ok;
    char summary[96];
} slot_info;

static struct {
    state_t st;
    bool quit;
    bool log_ok;
    bool am_ok;
    bool has_mod;
    femod mod;             // romfs mod data (Luma mod folder, sdmc:/3ds/FEAEditor/mod or the installed game)
    const char *mod_source;  // where it came from (shown on the slot screen)
    int bk_combo;          // Delete all: next step of Up, Down, Left, Right, B, A (0 = off)
    fe_save *orig;         // the slot as loaded (or last saved): edited numbers are colored against it

    fe_title titles[FS_MAX_TITLES];
    int title_count, title_sel;
    const fe_title *title;
    FS_Archive arch;
    bool arch_open;

    fs_entry entries[FS_MAX_ENTRIES];
    int entry_count;
    slot_info slots[SLOT_COUNT];
    int slot_sel;
    bool has_suspend;

    fe_save *save;
    int slot;
    bool dirty;
    int visible[FE_MAX_UNITS];
    int vis_count, unit_sel, unit_scroll;
    int edit_row;
    int skill_sel, skill_scroll;
    int class_sel, class_scroll;
    state_t class_return;  // screen to go back to from the class screen
    bool auto_backup;      // setting: back up the whole save before every write (default on)
    bool marriage_guard;   // setting: lower "S ready" to A for married units on load
    bool use_mod_data;     // setting: use romfs mod data when it is installed (default on)
    int sup_sel, sup_scroll;
    int army_sel;
    int inv_sel;                 // inventory slot
    int pick_sel, pick_scroll, pick_filter;  // item picker (filter: -1 all, else item type)
    int conv_sel, conv_scroll, conv_filter;
    int list_ids[FE_MAX_ITEMS + FE_FORGE_MAX + 8];
    int list_count;
    int extras_sel;
    fe_global global;     // the shared "Global" file, loaded on demand
    bool global_loaded;
    bool global_dirty;
    int global_row;
    struct { char name[32]; int support, gallery; bool clear, ok; } gbk[40];
    int gbk_count, gbk_sel, gbk_scroll;
    int add_sel, add_scroll, add_armed;
    int more_row, more_armed;    // per-unit "More" screen; armed: 1 revive, 2 remove
    int tools_sel, tools_armed;  // army-wide tools
    int rec_sel, rec_scroll, rec_row;
    int rp_row, rp_sel, rp_scroll, rp_count;  // record unit/class picker
    int rp_target;               // 0 = chapter record, 1 = barracks event
    int bk_ev_sel;               // barracks event slot
    bool bk_type_pick;           // choosing an event type from the list
    int bk_type_sel;
    int army_armed;
    bool rename_armed;
    int rp_ids[FE_MAX_CLASSES + FE_MAX_CHARS + 1];
    int map_sel, map_scroll;
    bool map_popup;
    int map_popup_sel;
    int imp_menu_sel;
    bool imp_no_supports;        // import as a logbook unit (no supports)
    int par_row;
    struct {
        int mode;        // 0 = a unit's inventory slot, 1 = convoy
        int slot;        // inventory slot (mode 0)
        int item;        // the item being forged (regular id, or forged id when editing)
        int forge;       // forge record being edited, or -1 for a new forge
        char name[48];
        int bonus[3];    // might, hit, crit
        int copies;      // convoy: how many to make
        int row;
        state_t back;
    } fg;
    fs_backup *bk;               // backup list (heap, FS_MAX_BACKUPS)
    int bk_count, bk_sel, bk_scroll, bk_armed;  // armed: 1 restore, 2 delete, 3 delete all
    fe_import_cand *imp;         // candidates (heap, IMPORT_MAX)
    int imp_count, imp_sel, imp_scroll, imp_armed;
    fe_save *imp_prev;           // preview: a copy of the save with the candidate imported
    int imp_prev_for, imp_prev_unit;
    int av_unit;   // index into save->units of the unit being styled
    int av_row;
    int lp_sel, lp_scroll;
    bool lp_heads_only;
    int lp_ids[FE_LOOK_PRESET_COUNT];
    int lp_count;  // index into the skill list (skill id = index + FE_SKILL_FIRST)
    char toast[96];
    int toast_frames;

    char msg_title[64];
    char msg[512];
    u32 msg_color;
    state_t msg_next;
} app;

//---------------------------------------------------------------------------
// Helpers
//---------------------------------------------------------------------------
static void fmt_gold(char *out, size_t len, uint32_t g)
{
    if (g >= 1000000)
        snprintf(out, len, "%lu,%03lu,%03lu", (unsigned long)(g / 1000000), (unsigned long)(g / 1000 % 1000), (unsigned long)(g % 1000));
    else if (g >= 1000)
        snprintf(out, len, "%lu,%03lu", (unsigned long)(g / 1000), (unsigned long)(g % 1000));
    else
        snprintf(out, len, "%lu", (unsigned long)g);
}

static void slot_file(int slot, char *out, size_t len)
{
    snprintf(out, len, "Chapter%d", slot);
}

static void show_message(const char *title, u32 color, state_t next, const char *fmt, ...)
{
    snprintf(app.msg_title, sizeof(app.msg_title), "%s", title);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app.msg, sizeof(app.msg), fmt, ap);
    va_end(ap);
    app.msg_color = color;
    app.msg_next = next;
    app.st = ST_MESSAGE;
    LOGI("message [%s]: %s", title, app.msg);
}

static void fatal(const char *fmt, ...)
{
    snprintf(app.msg_title, sizeof(app.msg_title), "Error");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app.msg, sizeof(app.msg), fmt, ap);
    va_end(ap);
    app.st = ST_FATAL;
    LOGE("fatal: %s", app.msg);
}

static bool ask_number(const char *hint, int initial, int max_digits, int *out)
{
    SwkbdState kb;
    char buf[16];
    char init[16];
    snprintf(init, sizeof(init), "%d", initial);

    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, max_digits);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, init);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof(buf));
    if (b != SWKBD_BUTTON_CONFIRM)
        return false;
    *out = atoi(buf);
    return true;
}

//---------------------------------------------------------------------------
// Loading
//---------------------------------------------------------------------------
#define SETTINGS_PATH APP_DIR "/settings.cfg"

static void settings_load(void)
{
    app.marriage_guard = true;  // default ON
    app.auto_backup = true;
    app.use_mod_data = true;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f)
        return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "marriage_guard=%d", &v) == 1)
            app.marriage_guard = v != 0;
        if (sscanf(line, "auto_backup=%d", &v) == 1)
            app.auto_backup = v != 0;
        if (sscanf(line, "use_mod_data=%d", &v) == 1)
            app.use_mod_data = v != 0;
    }
    fclose(f);
    LOGI("settings: marriage_guard=%d auto_backup=%d use_mod_data=%d", app.marriage_guard, app.auto_backup,
         app.use_mod_data);
}

static void settings_save(void)
{
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) {
        LOGE("could not write %s", SETTINGS_PATH);
        return;
    }
    fprintf(f, "marriage_guard=%d\n", app.marriage_guard ? 1 : 0);
    fprintf(f, "auto_backup=%d\n", app.auto_backup ? 1 : 0);
    fprintf(f, "use_mod_data=%d\n", app.use_mod_data ? 1 : 0);
    fclose(f);
    LOGI("settings saved: marriage_guard=%d auto_backup=%d use_mod_data=%d", app.marriage_guard, app.auto_backup,
         app.use_mod_data);
}

static void global_discard(void)
{
    if (app.global_loaded)
        fe_global_free(&app.global);
    app.global_loaded = false;
    app.global_dirty = false;
}

static bool global_ensure_loaded(void)
{
    if (app.global_loaded)
        return true;
    u8 *buf;
    u32 size;
    Result rc = fs_read_file(app.arch, "Global", &buf, &size);
    if (R_FAILED(rc)) {
        show_message("Global not found", CLR_ERR, ST_EXTRAS, "Could not read the Global file (rc=0x%08lX).",
                     (unsigned long)rc);
        return false;
    }
    char err[160];
    bool ok = fe_global_load(&app.global, buf, size, err, sizeof(err));
    free(buf);
    if (!ok) {
        show_message("Global unreadable", CLR_ERR, ST_EXTRAS, "%s", err);
        return false;
    }
    app.global_loaded = true;
    app.global_dirty = false;
    LOGI("Global loaded: game clear %d, support log %d/%d, gallery %d/%d", fe_global_game_clear(&app.global),
         fe_global_support_count(&app.global), FE_SUPPORT_LOG_ENTRIES, fe_global_gallery_count(&app.global),
         FE_GALLERY_ENTRIES);
    return true;
}

static void close_archive(void)
{
    global_discard();
    if (app.arch_open) {
        fs_close_save(app.arch);
        app.arch_open = false;
    }
}

static void refresh_slot_summary(int slot)
{
    slot_info *si = &app.slots[slot];
    char name[16];
    slot_file(slot, name, sizeof(name));
    si->present = fs_has_file(app.entries, app.entry_count, name);
    si->ok = false;
    if (!si->present) {
        snprintf(si->summary, sizeof(si->summary), "(empty)");
        return;
    }

    u8 *buf;
    u32 size;
    Result rc = fs_read_file(app.arch, name, &buf, &size);
    if (R_FAILED(rc)) {
        snprintf(si->summary, sizeof(si->summary), "read error 0x%08lX", (unsigned long)rc);
        return;
    }
    fe_save *s = malloc(sizeof(fe_save));
    char err[160];
    if (s && fe_save_load(s, buf, size, err, sizeof(err))) {
        int army = 0;
        for (int i = 0; i < s->unit_count; i++)
            if (s->units[i].group == FE_GROUP_ARMY || s->units[i].group == FE_GROUP_BLUE)
                army++;
        char gold[16];
        fmt_gold(gold, sizeof(gold), fe_get_gold(s));
        snprintf(si->summary, sizeof(si->summary), "%d units, %s G", army, gold);
        si->ok = true;
        LOGI("%s: %s", name, si->summary);
        fe_save_free(s);
    } else {
        snprintf(si->summary, sizeof(si->summary), "cannot read: %.80s", s ? err : "out of memory");
        LOGE("%s: %s", name, si->summary);
    }
    free(s);
    free(buf);
}

static int addable_count;  // add-unit list size (defined with the list below)

//---------------------------------------------------------------------------
// Romfs mod data: Luma mod folder, then sdmc:/3ds/FEAEditor/mod, then the installed game itself
// (a mod built into the game). The installed game counts only if its data differs from vanilla.
//---------------------------------------------------------------------------
#define MANUAL_MOD_DIR APP_DIR "/mod"

static bool try_mod_dir(const char *dir, const char *source)
{
    char merr[160];
    femod_free(&app.mod);
    if (!femod_load(&app.mod, dir, merr, sizeof(merr))) {
        LOGE("mod data in %s: %s", dir, merr);
        return false;
    }
    if (!app.mod.active) return false;
    app.mod_source = source;
    LOGI("mod data from %s (%s): %d characters, %d classes, %d items%s", source, dir, app.mod.char_count,
         app.mod.class_count, app.mod.item_count, app.mod.has_names ? "" : " (no name file)");
    return true;
}

static void load_mod_data(const char *luma_dir)
{
    femod_free(&app.mod);
    app.mod_source = NULL;
    addable_count = 0;  // the add-unit list depends on the character data
    char dir[96];
    struct stat st;
    bool found = false;
    if (app.has_mod) {
        ui_busy("Loading", "Reading the game mod's data...");
        snprintf(dir, sizeof(dir), "%s/romfs", luma_dir);
        found = try_mod_dir(dir, "Luma mod folder");
    }
    if (!found && stat(MANUAL_MOD_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        ui_busy("Loading", "Reading " MANUAL_MOD_DIR "...");
        found = try_mod_dir(MANUAL_MOD_DIR, "FEAEditor/mod");
    }
    if (!found) {
        ui_busy("Loading", "Checking the installed game's data...");
        Result rc = romfsMountFromTitle(app.title->title_id, app.title->media, "gamerom");
        if (R_SUCCEEDED(rc)) {
            found = try_mod_dir("gamerom:", "installed game");
            romfsUnmount("gamerom");
            if (found && !femod_differs_from_vanilla(&app.mod)) {
                LOGI("installed game data matches vanilla");
                femod_free(&app.mod);
                app.mod_source = NULL;
                found = false;
            }
        } else {
            LOGE("could not open the installed game's romfs (rc=0x%08lX)", (unsigned long)rc);
        }
    }
    femod_set_active(app.use_mod_data ? &app.mod : NULL);
}

static void open_title(int index)
{
    close_archive();
    app.title = &app.titles[index];
    LOGI("opening save of %016llX (%s, %s)", (unsigned long long)app.title->title_id,
         app.title->product, fs_media_name(app.title->media));

    Result rc = fs_open_save(app.title, &app.arch);
    if (R_FAILED(rc)) {
        fatal("Could not open the save data (rc=0x%08lX).\n\n"
              "Has the game been started and saved at least once?%s",
              (unsigned long)rc,
              app.title->media == MEDIATYPE_GAME_CARD ? "\nIs the cartridge firmly inserted?" : "");
        return;
    }
    app.arch_open = true;

    rc = fs_list_root(app.arch, app.entries, FS_MAX_ENTRIES, &app.entry_count);
    if (R_FAILED(rc)) {
        fatal("Could not list the save files (rc=0x%08lX).", (unsigned long)rc);
        return;
    }

    char mod_path[64];
    app.has_mod = fs_has_luma_mod(app.title, mod_path, sizeof(mod_path));
    load_mod_data(mod_path);

    app.has_suspend = fs_has_file(app.entries, app.entry_count, "Map0") ||
                      fs_has_file(app.entries, app.entry_count, "Map1");
    for (int i = 0; i < SLOT_COUNT; i++)
        refresh_slot_summary(i);
    app.slot_sel = 0;
    app.st = ST_PICK_SLOT;
}

static void build_visible_list(void)
{
    app.vis_count = 0;
    // Player units only: deployed, army, fallen
    for (int i = 0; i < app.save->unit_count; i++) {
        uint8_t g = app.save->units[i].group;
        if (g == FE_GROUP_BLUE || g == FE_GROUP_ARMY || g == FE_GROUP_DEAD)
            app.visible[app.vis_count++] = i;
    }
    app.unit_sel = 0;
    app.unit_scroll = 0;
}

//---------------------------------------------------------------------------
// The save as loaded: edited numbers are green above it, red below it
//---------------------------------------------------------------------------
#define NO_ORIGINAL (-100000)

static void remember_original(void)
{
    if (app.orig) { fe_save_free(app.orig); free(app.orig); app.orig = NULL; }
    fe_save *o = malloc(sizeof(fe_save));
    if (o && fe_save_clone(app.save, o)) app.orig = o;
    else free(o);
}

// The same unit in the original copy: same character id and per-unit id bytes (record +6..+9).
static const fe_unit *original_unit(const fe_unit *u)
{
    if (!app.orig || !u) return NULL;
    const uint8_t *a = app.save->data + u->off;
    for (int i = 0; i < app.orig->unit_count; i++) {
        const fe_unit *o = &app.orig->units[i];
        const uint8_t *b = app.orig->data + o->off;
        if (o->group == u->group && a[1] == b[1] && a[2] == b[2] && !memcmp(a + 6, b + 6, 4)) return o;
    }
    return NULL;
}

// Edit row r (0 = level, then the 8 stats) as it was when the slot was loaded.
static int original_value(const fe_unit *u, int r)
{
    const fe_unit *o = original_unit(u);
    if (!o) return NO_ORIGINAL;
    if (r == 0) return fe_unit_level(app.orig, o);
    fe_stats st;
    return fe_unit_stats(app.orig, o, &st) ? st.value[r - 1] : NO_ORIGINAL;
}

static u32 changed_color(int now, int was, u32 same)
{
    if (was == NO_ORIGINAL || now == was) return same;
    return now > was ? CLR_GOOD : CLR_ERR;
}

static void open_slot(int slot)
{
    char name[16];
    slot_file(slot, name, sizeof(name));
    ui_busy("Loading", name);

    u8 *buf;
    u32 size;
    Result rc = fs_read_file(app.arch, name, &buf, &size);
    if (R_FAILED(rc)) {
        show_message("Load failed", CLR_ERR, ST_PICK_SLOT, "Could not read %s (rc=0x%08lX).", name, (unsigned long)rc);
        return;
    }

    if (!app.save)
        app.save = malloc(sizeof(fe_save));
    else
        fe_save_free(app.save);

    char err[200];
    bool ok = app.save && fe_save_load(app.save, buf, size, err, sizeof(err));
    free(buf);
    if (!ok) {
        show_message("Load failed", CLR_ERR, ST_PICK_SLOT, "%s could not be opened:\n\n%s",
                     name, app.save ? err : "out of memory");
        return;
    }

    LOGI("opened %s: %d units, gold %lu", name, app.save->unit_count, (unsigned long)fe_get_gold(app.save));
    remember_original();
    app.slot = slot;
    app.dirty = false;
    build_visible_list();
    app.st = ST_UNITS;

    // "S ready" next to an existing marriage would undo that marriage in game
    char report[400];
    int fixed = app.marriage_guard ? fe_support_fix_pending_s(app.save, report, sizeof(report)) : 0;
    if (fixed > 0) {
        app.dirty = true;
        LOGI("marriage guard: %d S-ready pair(s) lowered to A:\n%s", fixed, report);
        show_message("Married units protected", CLR_ACCENT, ST_UNITS,
                     "%d pending S support%s would undo an existing marriage if watched, so %s lowered "
                     "to A:\n\n%s\nSave (START) to keep this, or back out without saving. "
                     "This check can be turned off on the slot screen (X).",
                     fixed, fixed == 1 ? "" : "s", fixed == 1 ? "it was" : "they were", report);
    } else if (fe_save_is_modded(app.save) && !femod_active()) {
        LOGI("save looks modded (convoy %d) but no mod data is in use", app.save->convoy_count);
        show_message("Modded save", CLR_WARN, ST_UNITS,
                     "This save comes from a romfs mod (it has characters or items the base game doesn't).\n\n"
                     "%s The mod's own characters, classes and items show as numbers. Forged weapons are "
                     "still read correctly.",
                     app.mod.active ? "Mod data is turned off (SELECT on the slot screen)."
                                    : "No mod data was found (Luma mod folder, sdmc:/3ds/FEAEditor/mod/ or the "
                                      "installed game).");
    }
}

static const fe_unit *selected_unit(void)
{
    if (app.vis_count == 0)
        return NULL;
    return &app.save->units[app.visible[app.unit_sel]];
}

//---------------------------------------------------------------------------
// Saving
//---------------------------------------------------------------------------
static void do_save(void)
{
    char name[16], err[300], dir[96] = "(backups are off)";
    slot_file(app.slot, name, sizeof(name));
    LOGI("=== save requested for %s (auto backup %s) ===", name, app.auto_backup ? "on" : "off");

    if (app.auto_backup) {
        ui_busy("Saving (1/3)", "Backing up the whole save to the SD card...");
        if (!fs_backup_save(app.arch, dir, sizeof(dir), err, sizeof(err))) {
            show_message("Backup failed - nothing written", CLR_ERR, ST_UNITS, "%s", err);
            return;
        }
    }

    ui_busy("Saving (2/3)", "Compressing and verifying...");
    u8 *out;
    size_t out_len;
    if (!fe_save_build(app.save, &out, &out_len, err, sizeof(err))) {
        show_message("Not saved", CLR_ERR, ST_UNITS, "The rebuilt save failed verification, so nothing was written.\n\n%s", err);
        return;
    }

    u8 *gout = NULL;
    size_t gout_len = 0;
    if (app.global_dirty && !fe_global_build(&app.global, &gout, &gout_len, err, sizeof(err))) {
        free(out);
        show_message("Not saved", CLR_ERR, ST_UNITS, "The rebuilt Global file failed verification, so nothing was written.\n\n%s", err);
        return;
    }

    ui_busy("Saving (3/3)", "Writing to the game's save data. Do not power off.");
    bool ok = fs_write_and_commit(app.arch, name, out, (u32)out_len, err, sizeof(err));
    free(out);
    if (!ok) {
        free(gout);
        show_message("Save failed", CLR_ERR, ST_UNITS, "%s\n\nBackup: %s", err, dir);
        return;
    }
    if (gout) {
        ok = fs_write_and_commit(app.arch, "Global", gout, (u32)gout_len, err, sizeof(err));
        free(gout);
        if (!ok) {
            show_message("Global not saved", CLR_ERR, ST_UNITS, "%s was saved, but writing Global failed:\n%s\n\nBackup: %s",
                         name, err, dir);
            return;
        }
        app.global_dirty = false;
    }

    app.dirty = false;
    remember_original();
    refresh_slot_summary(app.slot);
    show_message("Saved", CLR_GOOD, ST_UNITS,
                 "%s was written and verified.\n\nBackup of the original save:\n%s%s", name, dir,
                 app.has_suspend ? "\n\nNote: a suspended battle exists. Choosing Continue in-game loads that, "
                                   "not this slot." : "");
}

//---------------------------------------------------------------------------
// Edits
//---------------------------------------------------------------------------
static void edit_value(const fe_unit *u, int row, int delta, bool to_max, bool ask)
{
    fe_save *s = app.save;
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    if (row == 0) {
        int old = fe_unit_level(s, u);
        int want = old + delta;
        if (to_max)
            want = fe_unit_level_cap(s, u);
        if (ask && !ask_number("Level", old, 2, &want))
            return;
        int got = fe_unit_set_level(s, u, want);
        if (got != old) {
            app.dirty = true;
            LOGI("edit %s: Level %d -> %d", name, old, got);
        }
        return;
    }

    int stat = row - 1;
    fe_stats st;
    if (!fe_unit_stats(s, u, &st))
        return;
    int old = st.value[stat];
    int want = old + delta;
    if (to_max)
        want = st.cap[stat];
    if (ask) {
        // The keyboard takes the number as the game shows it (including buffs)
        int shown;
        if (!ask_number(fe_stat_names[stat], fe_stat_shown(&st, stat), 3, &shown))
            return;
        want = shown - st.buff[stat];
    }
    int got = fe_unit_set_stat(s, u, stat, want);
    if (got >= 0 && got != old) {
        app.dirty = true;
        if (st.buff[stat])
            LOGI("edit %s: %s %d -> %d (in game %d -> %d incl. +%d bonus)", name, fe_stat_names[stat], old, got,
                 old + st.buff[stat], got + st.buff[stat], st.buff[stat]);
        else
            LOGI("edit %s: %s %d -> %d", name, fe_stat_names[stat], old, got);
    }
}

static void edit_renown(void)
{
    int old = (int)fe_get_renown(app.save);
    int want;
    if (!ask_number("Renown (max 99999)", old, 5, &want))
        return;
    if (want < 0)
        want = 0;
    fe_set_renown(app.save, (uint32_t)want);
    if ((int)fe_get_renown(app.save) != old) {
        app.dirty = true;
        LOGI("edit renown: %d -> %lu", old, (unsigned long)fe_get_renown(app.save));
    }
}

static void edit_gold(void)
{
    int old = (int)fe_get_gold(app.save);
    int want;
    if (!ask_number("Gold (max 999999)", old, 6, &want))
        return;
    if (want < 0)
        want = 0;
    fe_set_gold(app.save, (uint32_t)want);
    if ((int)fe_get_gold(app.save) != old) {
        app.dirty = true;
        LOGI("edit gold: %d -> %lu", old, (unsigned long)fe_get_gold(app.save));
    }
}

//---------------------------------------------------------------------------
// Screens
//---------------------------------------------------------------------------
static const ui_button BTN_EDIT   = {2, 160, 61, 34, "Stats"};
static const ui_button BTN_CLASSB = {65, 160, 61, 34, "Class"};
static const ui_button BTN_SKILLS = {128, 160, 61, 34, "Skills"};
static const ui_button BTN_SUPP   = {191, 160, 61, 34, "Supp"};
static const ui_button BTN_ITEMS  = {254, 160, 64, 34, "Items"};
static const ui_button BTN_GOLD   = {2, 200, 61, 36, "Army"};
static const ui_button BTN_CONVOY = {65, 200, 61, 36, "Convoy"};
static const ui_button BTN_EXTRAS = {128, 200, 61, 36, "Extras"};
static const ui_button BTN_SAVE   = {191, 200, 61, 36, "Save"};
static const ui_button BTN_BACK   = {254, 200, 64, 36, "Back"};
static const ui_button BTN_DONE  = {242, 206, 70, 30, "Done"};
static const ui_button BTN_CLASS  = {152, 206, 84, 30, "Class"};
static const ui_button BTN_MORE   = {78, 206, 70, 30, "More"};
static const ui_button BTN_RENAME = {4, 206, 70, 30, "Rename"};
static const ui_button BTN_YES   = {40, 180, 110, 44, "Yes (A)"};
static const ui_button BTN_NO    = {170, 180, 110, 44, "No (B)"};
static const ui_button BTN_CF_BACKUP = {40, 110, 240, 40, NULL};
static const ui_button BTN_OK    = {105, 180, 110, 44, "OK (A)"};

static void draw_unit_stats_panel(const fe_unit *u, float y)
{
    fe_save *s = app.save;
    fe_stats st;
    if (!fe_unit_stats(s, u, &st)) {
        ui_text(10, y, 0.5f, CLR_DIM, "No stat data for this unit (unknown character/class).");
        return;
    }
    bool any_buff = false;
    for (int i = 0; i < FE_STAT_COUNT; i++) any_buff |= st.buff[i] != 0;
    if (any_buff)
        ui_text(10, y + 44, 0.42f, CLR_ACCENT, "Yellow = includes a tonic/skill/barracks bonus");
    for (int i = 0; i < FE_STAT_COUNT; i++) {
        float x = 10 + (i % 4) * 77;
        float yy = y + (i / 4) * 20;
        ui_text(x, yy, 0.5f, CLR_DIM, "%s", fe_stat_names[i]);
        ui_text(x + 30, yy, 0.5f, st.buff[i] ? CLR_ACCENT : (st.value[i] >= st.cap[i] ? CLR_GOOD : CLR_TEXT),
                "%d", fe_stat_shown(&st, i));
    }
}

static void screen_pick_title(u32 down, const touchPosition *touch)
{
    (void)touch;
    if (down & KEY_UP) app.title_sel = (app.title_sel + app.title_count - 1) % app.title_count;
    if (down & KEY_DOWN) app.title_sel = (app.title_sel + 1) % app.title_count;
    if (down & KEY_A) { open_title(app.title_sel); return; }
    if (down & (KEY_B | KEY_START)) { app.quit = true; return; }

    ui_begin();
    ui_top();
    ui_text(12, 8, 0.7f, CLR_ACCENT, "Awakening Save Editor");
    ui_text(12, 36, 0.55f, CLR_TEXT, "More than one copy found. Choose one:");
    for (int i = 0; i < app.title_count; i++) {
        float y = 64 + i * 22;
        if (i == app.title_sel) ui_rect(8, y - 2, TOP_W - 16, 21, CLR_SEL);
        ui_text(14, y, 0.55f, CLR_TEXT, "%s  %s  %016llX", fs_media_name(app.titles[i].media),
                app.titles[i].product, (unsigned long long)app.titles[i].title_id);
    }
    ui_bottom();
    ui_text(10, 10, 0.5f, CLR_DIM, "A: open    B/START: exit");
    ui_end();
}

static void open_backups(void);
static void open_forge(int mode, int slot, int item, state_t back);

static void screen_pick_slot(u32 down, const touchPosition *touch)
{
    if (down & KEY_UP) app.slot_sel = (app.slot_sel + SLOT_COUNT - 1) % SLOT_COUNT;
    if (down & KEY_DOWN) app.slot_sel = (app.slot_sel + 1) % SLOT_COUNT;
    if (down & KEY_TOUCH) {
        for (int i = 0; i < SLOT_COUNT; i++) {
            ui_button b = {8, 40 + i * 46.0f, BOTTOM_W - 16, 40, NULL};
            if (ui_button_hit(&b, touch)) {
                app.slot_sel = i;
                down |= KEY_A;
            }
        }
    }
    if (down & KEY_A) {
        if (app.slots[app.slot_sel].ok) {
            open_slot(app.slot_sel);
            return;
        }
    }
    if (down & KEY_B) {
        if (app.title_count > 1) {
            close_archive();
            app.st = ST_PICK_TITLE;
        } else {
            app.quit = true;
        }
        return;
    }
    if (down & KEY_START) { app.quit = true; return; }
    if (down & KEY_X) {
        app.marriage_guard = !app.marriage_guard;
        settings_save();
    }
    if (down & KEY_TOUCH) {
        ui_button bk = {214, 4, 98, 28, NULL};
        if (ui_button_hit(&bk, touch)) down |= KEY_Y;
        ui_button md = {0, 196, BOTTOM_W, 24, NULL};
        if (app.mod.active && ui_button_hit(&md, touch)) down |= KEY_SELECT;
    }
    if ((down & KEY_SELECT) && app.mod.active) {
        app.use_mod_data = !app.use_mod_data;
        femod_set_active(app.use_mod_data ? &app.mod : NULL);
        addable_count = 0;
        settings_save();
        for (int i = 0; i < SLOT_COUNT; i++)
            refresh_slot_summary(i);
    }
    if (down & KEY_Y) { open_backups(); return; }

    ui_begin();
    ui_top();
    ui_text(12, 8, 0.7f, CLR_ACCENT, "Awakening Save Editor");
    ui_text(12, 34, 0.5f, CLR_DIM, "%s  %s  %016llX", fs_media_name(app.title->media), app.title->product,
            (unsigned long long)app.title->title_id);
    for (int i = 0; i < SLOT_COUNT; i++) {
        float y = 64 + i * 30;
        if (i == app.slot_sel) ui_rect(8, y - 4, TOP_W - 16, 28, CLR_SEL);
        ui_text(16, y, 0.6f, app.slots[i].ok ? CLR_TEXT : CLR_DIM, "Slot %d", i + 1);
        ui_text(90, y + 2, 0.55f, app.slots[i].ok ? CLR_TEXT : (app.slots[i].present ? CLR_ERR : CLR_DIM),
                "%s", app.slots[i].summary);
    }
    if (app.has_suspend)
        ui_text_wrap(12, 166, 0.45f, CLR_WARN, TOP_W - 24,
                     "A suspended battle (Map save) exists. If you pick Continue in-game it loads the "
                     "battle, not the slot you edit here. Load the slot from the title menu instead.");

    ui_bottom();
    ui_text(10, 10, 0.46f, CLR_DIM, "Tap a slot or press A.");
    ui_button bkb = {214, 4, 98, 28, "Backups (Y)"};
    ui_button_draw(&bkb, true);
    ui_text(10, 182, 0.45f, app.marriage_guard ? CLR_GOOD : CLR_DIM, "X: Marriage guard on load: %s",
            app.marriage_guard ? "ON" : "OFF");
    if (app.mod.active) {
        char mtxt[160];
        snprintf(mtxt, sizeof(mtxt), "SELECT: mod data (%s): %s", app.mod_source ? app.mod_source : "mod",
                 app.use_mod_data ? "ON" : "OFF - vanilla tables");
        ui_text_wrap(10, 198, 0.4f, app.use_mod_data ? CLR_GOOD : CLR_DIM, BOTTOM_W - 20, mtxt);
    }
    else if (app.has_mod)
        ui_text_wrap(10, 198, 0.4f, CLR_WARN, BOTTOM_W - 20,
                     "Game mod detected: stats shown are for the unmodded game. Edits still apply 1:1.");
    if (!app.log_ok)
        ui_text(10, 222, 0.45f, CLR_WARN, "Warning: cannot write debug.log to the SD card.");
    for (int i = 0; i < SLOT_COUNT; i++) {
        ui_button b = {8, 40 + i * 46.0f, BOTTOM_W - 16, 40, NULL};
        char label[24];
        snprintf(label, sizeof(label), "Slot %d", i + 1);
        b.label = label;
        ui_button_draw(&b, app.slots[i].ok);
    }
    ui_end();
}

static void screen_units(u32 down, u32 repeat, const touchPosition *touch)
{
    if (app.vis_count > 0) {
        if (repeat & KEY_UP) app.unit_sel = (app.unit_sel + app.vis_count - 1) % app.vis_count;
        if (repeat & KEY_DOWN) app.unit_sel = (app.unit_sel + 1) % app.vis_count;
        if (repeat & KEY_LEFT) app.unit_sel = app.unit_sel >= LIST_ROWS ? app.unit_sel - LIST_ROWS : 0;
        if (repeat & KEY_RIGHT) app.unit_sel = app.unit_sel + LIST_ROWS < app.vis_count ? app.unit_sel + LIST_ROWS : app.vis_count - 1;
    }
    if (app.unit_sel < app.unit_scroll) app.unit_scroll = app.unit_sel;
    if (app.unit_sel >= app.unit_scroll + LIST_ROWS) app.unit_scroll = app.unit_sel - LIST_ROWS + 1;

    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_EDIT, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_SKILLS, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_SUPP, touch)) down |= KEY_SELECT;
        ui_button class_line = {0, 26, BOTTOM_W, 22, NULL};
        if (ui_button_hit(&class_line, touch) || ui_button_hit(&BTN_CLASSB, touch)) down |= KEY_L;
        if (ui_button_hit(&BTN_ITEMS, touch)) down |= KEY_R;
        if (ui_button_hit(&BTN_EXTRAS, touch)) { app.extras_sel = 0; app.st = ST_EXTRAS; return; }
        if (ui_button_hit(&BTN_CONVOY, touch)) { app.conv_sel = 0; app.conv_scroll = 0; app.conv_filter = -1; app.list_count = 0; app.st = ST_CONVOY; return; }
        if (ui_button_hit(&BTN_GOLD, touch)) down |= KEY_Y;
        if (ui_button_hit(&BTN_SAVE, touch)) down |= KEY_START;
        if (ui_button_hit(&BTN_BACK, touch)) down |= KEY_B;
    }
    if ((down & KEY_A) && app.vis_count > 0) { app.edit_row = 0; app.toast_frames = 0; app.rename_armed = false; app.st = ST_EDIT; return; }
    if ((down & KEY_R) && app.vis_count > 0) {
        app.inv_sel = 0;
        app.toast_frames = 0;
        app.st = ST_ITEMS;
        return;
    }
    if ((down & KEY_L) && app.vis_count > 0) {
        const fe_unit *cu = selected_unit();
        int c = fe_unit_class_id(app.save, cu);
        app.class_sel = c < fe_class_count() ? c : 0;
        app.class_scroll = app.class_sel;
        app.toast_frames = 0;
        app.class_return = ST_UNITS;
        app.st = ST_CLASS;
        return;
    }
    if ((down & KEY_SELECT) && app.vis_count > 0) {
        app.sup_sel = 0;
        app.sup_scroll = 0;
        app.toast_frames = 0;
        app.st = ST_SUPPORTS;
        return;
    }
    if ((down & KEY_X) && app.vis_count > 0) {
        app.skill_sel = 0;
        app.skill_scroll = 0;
        app.toast_frames = 0;
        app.st = ST_SKILLS;
        return;
    }
    if (down & KEY_Y) { app.army_sel = 0; app.st = ST_ARMY; return; }
    if (down & KEY_START) {
        if (app.dirty) app.st = ST_CONFIRM_SAVE;
        else show_message("Nothing to save", CLR_DIM, ST_UNITS, "No changes have been made to this slot.");
        return;
    }
    if (down & KEY_B) {
        app.st = app.dirty ? ST_CONFIRM_DISCARD : ST_PICK_SLOT;
        return;
    }

    fe_save *s = app.save;
    char gold[16];
    fmt_gold(gold, sizeof(gold), fe_get_gold(s));

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Slot %d%s%s%s%s", app.slot + 1, app.dirty ? "  * unsaved" : "",
            femod_active() ? "  [mod]" : app.has_mod ? "  [mod off]" : "", "", "");
    char ren[16];
    fmt_gold(ren, sizeof(ren), fe_get_renown(s));
    ui_text_right(TOP_W - 8, 3, 0.5f, CLR_TEXT, "Gold %s  Renown %s", gold, ren);

    for (int r = 0; r < LIST_ROWS && app.unit_scroll + r < app.vis_count; r++) {
        int vi = app.unit_scroll + r;
        const fe_unit *u = &s->units[app.visible[vi]];
        float y = 28 + r * ROW_H;
        if (vi == app.unit_sel) ui_rect(4, y - 1, TOP_W - 8, ROW_H, CLR_SEL);
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        u32 c = u->group == FE_GROUP_DEAD ? CLR_DIM : CLR_TEXT;
        ui_text(10, y, 0.5f, c, "%s", name);
        ui_text(140, y, 0.5f, CLR_DIM, "%s", fe_class_name(fe_unit_class_id(s, u)));
        ui_text(290, y, 0.5f, c, "Lv %d", fe_unit_level(s, u));
        if (u->group != FE_GROUP_ARMY)
            ui_text(340, y, 0.45f, u->group == FE_GROUP_DEAD ? CLR_ERR : CLR_DIM, "%s", fe_group_name(u->group));
    }
    if (app.vis_count == 0)
        ui_text(10, 40, 0.55f, CLR_DIM, "No player units in this save.");
    ui_text(8, 222, 0.42f, CLR_DIM, "%d/%d A:stats L:class R:items X:skills SEL:supp Y:army",
            app.vis_count ? app.unit_sel + 1 : 0, app.vis_count);

    ui_bottom();
    const fe_unit *u = selected_unit();
    if (u) {
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        ui_text(10, 8, 0.6f, CLR_ACCENT, "%s", name);
        ui_text(10, 30, 0.5f, CLR_TEXT, "%s (L)   Lv %d / %d   EXP %d", fe_class_name(fe_unit_class_id(s, u)),
                fe_unit_level(s, u), fe_unit_level_cap(s, u), fe_unit_exp(s, u));
        draw_unit_stats_panel(u, 60);
        int eq[FE_EQUIP_SLOTS];
        fe_unit_equipped(s, u, eq);
        char line[160] = "";
        size_t o = 0;
        for (int k = 0; k < FE_EQUIP_SLOTS; k++)
            if (eq[k] && eq[k] < FE_SKILL_COUNT && o < sizeof(line))
                o += snprintf(line + o, sizeof(line) - o, "%s%s", o ? ", " : "", fe_skills[eq[k]].name);
        ui_text(10, 116, 0.42f, CLR_DIM, "Skills (%d learned):", fe_unit_learned_count(s, u));
        ui_text_wrap(10, 130, 0.42f, CLR_TEXT, BOTTOM_W - 20, o ? line : "(none equipped)");
    }
    ui_button_draw(&BTN_EDIT, app.vis_count > 0);
    ui_button_draw(&BTN_SKILLS, app.vis_count > 0);
    ui_button_draw(&BTN_CLASSB, app.vis_count > 0);
    ui_button_draw(&BTN_ITEMS, app.vis_count > 0);
    ui_button_draw(&BTN_CONVOY, true);
    ui_button_draw(&BTN_EXTRAS, true);
    ui_button_draw(&BTN_SUPP, app.vis_count > 0);
    ui_button_draw(&BTN_GOLD, true);
    ui_button_draw(&BTN_SAVE, app.dirty);
    ui_button_draw(&BTN_BACK, true);
    ui_end();
}

static float edit_row_y(int row) { return 4 + row * 22.0f; }

static void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool ask_text(const char *hint, const char *initial, int max_len, char *out, size_t outlen);
static int logbook_source(int except);

// Rename: only units with logbook data (Avatars, imported Avatars, heroes) carry a name.
static void rename_unit(void)
{
    fe_save *s = app.save;
    int idx = app.visible[app.unit_sel];
    const fe_unit *u = &s->units[idx];
    char cur[48], buf[64];
    fe_unit_name(s, u, cur, sizeof(cur));
    if (u->log_off < 0) {
        // Giving regular characters name data works, but the game then shows Robin's portrait
        // (hardware test 2026-10-02), so only units that already have a name can be renamed.
        toast("Only Avatars and logbook/hero units can be renamed");
        return;
    }
    if (!ask_text("Name (max 12)", cur, FE_NAME_MAX, buf, sizeof(buf)) || !buf[0]) return;
    fe_unit_set_name(s, u, buf);
    app.dirty = true;
    LOGI("rename: %s -> %s", cur, buf);
    toast("Renamed to %s", buf);
}

static void screen_edit(u32 down, u32 repeat, const touchPosition *touch)
{
    const fe_unit *u = selected_unit();
    fe_save *s = app.save;
    fe_stats st;
    bool has_stats = fe_unit_stats(s, u, &st);
    int rows = has_stats ? EDIT_ROWS : 1;
    if (app.edit_row >= rows) app.edit_row = 0;

    if (repeat & KEY_UP) app.edit_row = (app.edit_row + rows - 1) % rows;
    if (repeat & KEY_DOWN) app.edit_row = (app.edit_row + 1) % rows;
    if (repeat & KEY_LEFT) edit_value(u, app.edit_row, -1, false, false);
    if (repeat & KEY_RIGHT) edit_value(u, app.edit_row, +1, false, false);
    if (repeat & KEY_L) edit_value(u, app.edit_row, -5, false, false);
    if (repeat & KEY_R) edit_value(u, app.edit_row, +5, false, false);
    if (down & KEY_X) edit_value(u, app.edit_row, 0, true, false);
    if (down & KEY_Y) edit_value(u, app.edit_row, 0, false, true);

    if (down & KEY_TOUCH) {
        for (int r = 0; r < rows; r++) {
            float y = edit_row_y(r);
            ui_button minus = {150, y, 40, 20, NULL}, plus = {196, y, 40, 20, NULL}, max = {242, y, 70, 20, NULL};
            ui_button label = {0, y, 146, 20, NULL};
            if (ui_button_hit(&minus, touch)) { app.edit_row = r; edit_value(u, r, -1, false, false); }
            if (ui_button_hit(&plus, touch)) { app.edit_row = r; edit_value(u, r, +1, false, false); }
            if (ui_button_hit(&max, touch)) { app.edit_row = r; edit_value(u, r, 0, true, false); }
            if (ui_button_hit(&label, touch)) { app.edit_row = r; edit_value(u, r, 0, false, true); }
        }
        if (ui_button_hit(&BTN_DONE, touch)) down |= KEY_B;
        if (ui_button_hit(&BTN_CLASS, touch)) down |= KEY_SELECT;
        if (ui_button_hit(&BTN_MORE, touch)) down |= KEY_START;
        if (u->log_off >= 0 && ui_button_hit(&BTN_RENAME, touch)) { rename_unit(); u = selected_unit(); }
        else app.rename_armed = false;
    }
    if (down & (KEY_START | KEY_SELECT | KEY_B | KEY_A)) app.rename_armed = false;
    if (down & KEY_START) {
        app.more_row = 0;
        app.more_armed = 0;
        app.toast_frames = 0;
        app.st = ST_MORE;
        return;
    }
    if (down & KEY_SELECT) {
        app.class_sel = fe_unit_class_id(s, u) < fe_class_count() ? fe_unit_class_id(s, u) : 0;
        app.class_scroll = app.class_sel;
        app.toast_frames = 0;
        app.class_return = ST_EDIT;
        app.st = ST_CLASS;
        return;
    }
    if (down & (KEY_B | KEY_A)) { app.st = ST_UNITS; return; }

    has_stats = fe_unit_stats(s, u, &st);  // refresh after edits
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));
    int lv = fe_unit_level(s, u), lvcap = fe_unit_level_cap(s, u);

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s", name);
    ui_text_right(TOP_W - 8, 3, 0.55f, CLR_DIM, "%s%s", fe_class_name(fe_unit_class_id(s, u)),
                  has_stats && st.limit_breaker ? "  (Limit Breaker)" : "");

    for (int r = 0; r < rows; r++) {
        float y = 28 + r * 19;
        int val = r == 0 ? lv : st.value[r - 1];
        int cap = r == 0 ? lvcap : st.cap[r - 1];
        int buff = r == 0 ? 0 : st.buff[r - 1];
        int was = original_value(u, r);
        if (r == app.edit_row) ui_rect(4, y - 1, TOP_W - 8, 18, CLR_SEL);
        ui_text(12, y, 0.55f, CLR_TEXT, "%s", r == 0 ? "Level" : fe_stat_names[r - 1]);
        ui_text_right(84, y, 0.55f, changed_color(val, was, buff ? CLR_ACCENT : CLR_TEXT), "%d", val + buff);
        if (was != NO_ORIGINAL && was != val)
            ui_text(buff ? 170 : 132, y + 2, 0.42f, CLR_DIM, "was %d", was);
        ui_text(90, y, 0.55f, CLR_DIM, "/ %d", cap);
        if (buff)
            ui_text(132, y + 2, 0.42f, CLR_ACCENT, "%d+%d", val, buff);
        // bar
        float bw = 200.0f;
        ui_rect(180, y + 4, bw, 8, CLR_PANEL);
        float frac = cap > 0 ? (float)val / cap : 0;
        if (frac > 1) frac = 1;
        ui_rect(180, y + 4, bw * frac, 8, val >= cap ? CLR_GOOD : CLR_ACCENT);
    }
    if (!has_stats)
        ui_text_wrap(12, 60, 0.5f, CLR_WARN, TOP_W - 24,
                     "This unit's character or class is not in the stat tables (story enemy or modded save), "
                     "so only the level can be edited.");
    if (app.toast_frames > 0) {
        app.toast_frames--;
        ui_text(8, 204, 0.46f, app.rename_armed ? CLR_WARN : CLR_GOOD, "%s", app.toast);
    } else {
        ui_text(8, 206, 0.42f, CLR_DIM, "Left/Right: -1/+1  L/R: -5/+5  X: max  Y/tap name: type");
    }
    ui_text(8, 222, 0.42f, CLR_DIM, "SELECT: class  START: more (weapon ranks...)  B: back");

    ui_bottom();
    for (int r = 0; r < rows; r++) {
        float y = edit_row_y(r);
        int buff = r == 0 ? 0 : st.buff[r - 1];
        int raw = r == 0 ? lv : st.value[r - 1];
        int val = raw + buff;
        if (r == app.edit_row) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        ui_text(8, y + 1, 0.5f, CLR_TEXT, "%s", r == 0 ? "Level" : fe_stat_names[r - 1]);
        ui_text_right(140, y + 1, 0.5f, changed_color(raw, original_value(u, r), buff ? CLR_ACCENT : CLR_TEXT), "%d", val);
        ui_button minus = {150, y, 40, 20, "-"}, plus = {196, y, 40, 20, "+"}, max = {242, y, 70, 20, "Max"};
        ui_button_draw(&minus, true);
        ui_button_draw(&plus, true);
        ui_button_draw(&max, true);
    }
    if (u->log_off >= 0) ui_button_draw(&BTN_RENAME, true);  // only Avatars and logbook/hero units have a name
    ui_button_draw(&BTN_MORE, true);
    ui_button_draw(&BTN_CLASS, true);
    ui_button_draw(&BTN_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Skills screen: learned list on the bottom (touch), equipped slots on top
//---------------------------------------------------------------------------
#define SKILL_ROWS   10
#define SKILL_TOTAL  (FE_SKILL_LAST - FE_SKILL_FIRST + 1)

static void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app.toast, sizeof(app.toast), fmt, ap);
    va_end(ap);
    app.toast_frames = 150;
}

static void skill_toggle_learned(const fe_unit *u, int id)
{
    char name[48];
    fe_unit_name(app.save, u, name, sizeof(name));
    bool now = !fe_unit_is_learned(app.save, u, id);
    bool was_equipped = fe_unit_is_equipped(app.save, u, id);
    fe_unit_set_learned(app.save, u, id, now);
    app.dirty = true;
    LOGI("skill edit %s: %s %s%s", name, now ? "learned" : "forgot", fe_skills[id].name,
         (!now && was_equipped) ? " (also unequipped)" : "");
    if (!now && was_equipped)
        toast("%s unequipped and forgotten", fe_skills[id].name);
}

static void skill_toggle_equipped(const fe_unit *u, int id)
{
    char name[48];
    fe_unit_name(app.save, u, name, sizeof(name));
    if (fe_unit_is_equipped(app.save, u, id)) {
        fe_unit_unequip(app.save, u, id);
        app.dirty = true;
        LOGI("skill edit %s: unequipped %s (still learned)", name, fe_skills[id].name);
        toast("%s unequipped (still learned)", fe_skills[id].name);
        return;
    }
    bool was_learned = fe_unit_is_learned(app.save, u, id);
    int slot = fe_unit_equip(app.save, u, id);
    if (slot < 0) {
        toast("All 5 slots are full. Unequip a skill first.");
        return;
    }
    app.dirty = true;
    LOGI("skill edit %s: equipped %s in slot %d%s", name, fe_skills[id].name, slot + 1,
         was_learned ? "" : " (and learned it)");
    toast("%s equipped in slot %d%s", fe_skills[id].name, slot + 1, was_learned ? "" : " and learned");
}

static const ui_button BTN_SK_PREV = {4, 202, 54, 34, "<<"};
static const ui_button BTN_SK_NEXT = {62, 202, 54, 34, ">>"};
static const ui_button BTN_SK_ALL  = {120, 202, 110, 34, "Learn all"};
static const ui_button BTN_SK_DONE = {234, 202, 82, 34, "Done"};

static void screen_skills(u32 down, u32 repeat, const touchPosition *touch)
{
    const fe_unit *u = selected_unit();
    fe_save *s = app.save;

    if (repeat & KEY_UP) app.skill_sel = (app.skill_sel + SKILL_TOTAL - 1) % SKILL_TOTAL;
    if (repeat & KEY_DOWN) app.skill_sel = (app.skill_sel + 1) % SKILL_TOTAL;
    if (repeat & (KEY_LEFT | KEY_L)) app.skill_sel = app.skill_sel >= SKILL_ROWS ? app.skill_sel - SKILL_ROWS : 0;
    if (repeat & (KEY_RIGHT | KEY_R))
        app.skill_sel = app.skill_sel + SKILL_ROWS < SKILL_TOTAL ? app.skill_sel + SKILL_ROWS : SKILL_TOTAL - 1;

    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_SK_PREV, touch))
            app.skill_sel = app.skill_scroll >= SKILL_ROWS ? app.skill_scroll - SKILL_ROWS : 0;
        if (ui_button_hit(&BTN_SK_NEXT, touch))
            app.skill_sel = app.skill_scroll + SKILL_ROWS < SKILL_TOTAL ? app.skill_scroll + SKILL_ROWS : SKILL_TOTAL - 1;
        if (ui_button_hit(&BTN_SK_ALL, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_SK_DONE, touch)) down |= KEY_B;
        for (int r = 0; r < SKILL_ROWS && app.skill_scroll + r < SKILL_TOTAL; r++) {
            float y = 2 + r * 20.0f;
            ui_button learn = {0, y, 236, 19, NULL}, equip = {240, y, 76, 19, NULL};
            int id = app.skill_scroll + r + FE_SKILL_FIRST;
            if (ui_button_hit(&learn, touch)) { app.skill_sel = app.skill_scroll + r; skill_toggle_learned(u, id); }
            if (ui_button_hit(&equip, touch)) { app.skill_sel = app.skill_scroll + r; skill_toggle_equipped(u, id); }
        }
    }
    if (app.skill_sel < app.skill_scroll) app.skill_scroll = app.skill_sel;
    if (app.skill_sel >= app.skill_scroll + SKILL_ROWS) app.skill_scroll = app.skill_sel - SKILL_ROWS + 1;

    int sel_id = app.skill_sel + FE_SKILL_FIRST;
    if (down & KEY_A) skill_toggle_learned(u, sel_id);
    if (down & KEY_Y) skill_toggle_equipped(u, sel_id);
    if (down & KEY_X) {
        int added = fe_unit_learn_all(s, u);
        if (added) {
            app.dirty = true;
            char nm[48];
            fe_unit_name(s, u, nm, sizeof(nm));
            LOGI("skill edit %s: learn all (+%d)", nm, added);
            toast("Learned %d more skills (now all %d)", added, SKILL_TOTAL);
        } else {
            toast("Already knows all %d skills", SKILL_TOTAL);
        }
    }
    if (down & KEY_B) { app.st = ST_UNITS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    char name[48];
    fe_unit_name(s, u, name, sizeof(name));
    int eq[FE_EQUIP_SLOTS];
    fe_unit_equipped(s, u, eq);

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s - Skills", name);
    ui_text_right(TOP_W - 8, 3, 0.55f, CLR_TEXT, "Learned %d / %d", fe_unit_learned_count(s, u), SKILL_TOTAL);

    ui_text(12, 30, 0.5f, CLR_DIM, "Equipped (in game you can swap in any learned skill):");
    for (int k = 0; k < FE_EQUIP_SLOTS; k++) {
        float y = 50 + k * 19;
        if (eq[k] && eq[k] == sel_id) ui_rect(8, y - 1, TOP_W - 16, 18, CLR_SEL);
        ui_text(16, y, 0.55f, CLR_DIM, "%d", k + 1);
        ui_text(36, y, 0.55f, eq[k] ? CLR_TEXT : CLR_DIM, "%s",
                eq[k] ? (eq[k] < FE_SKILL_COUNT ? fe_skills[eq[k]].name : "?") : "-");
    }

    bool learned = fe_unit_is_learned(s, u, sel_id), equipped = fe_unit_is_equipped(s, u, sel_id);
    ui_text(12, 150, 0.6f, CLR_ACCENT, "%s", fe_skills[sel_id].name);
    ui_text(12, 170, 0.48f, learned ? CLR_GOOD : CLR_DIM, "%s%s%s", learned ? "Learned" : "Not learned",
            equipped ? ", equipped" : "", fe_skills[sel_id].dlc ? "   (DLC skill)" : "");
    if (app.toast_frames > 0)
        ui_text(12, 190, 0.48f, CLR_WARN, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "A: learn/forget  Y: equip/unequip  X: learn all  L/R: page  B: back");

    ui_bottom();
    for (int r = 0; r < SKILL_ROWS && app.skill_scroll + r < SKILL_TOTAL; r++) {
        int i = app.skill_scroll + r, id = i + FE_SKILL_FIRST;
        float y = 2 + r * 20.0f;
        bool l = fe_unit_is_learned(s, u, id), e = fe_unit_is_equipped(s, u, id);
        if (i == app.skill_sel) ui_rect(0, y - 1, BOTTOM_W, 20, CLR_SEL);
        ui_rect(6, y + 2, 14, 14, l ? CLR_GOOD : CLR_PANEL);
        ui_text(26, y + 1, 0.48f, l ? CLR_TEXT : CLR_DIM, "%s", fe_skills[id].name);
        ui_button eb = {240, y, 76, 18, e ? "Unequip" : "Equip"};
        ui_button_draw(&eb, true);
    }
    ui_button_draw(&BTN_SK_PREV, app.skill_scroll > 0);
    ui_button_draw(&BTN_SK_NEXT, app.skill_scroll + SKILL_ROWS < SKILL_TOTAL);
    ui_button_draw(&BTN_SK_ALL, true);
    ui_button_draw(&BTN_SK_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Class screen: every class is allowed; risky ones are only tagged
//---------------------------------------------------------------------------
#define CLASS_ROWS 10

static const ui_button BTN_CL_PREV   = {4, 202, 54, 34, "<<"};
static const ui_button BTN_CL_NEXT   = {62, 202, 54, 34, ">>"};
static const ui_button BTN_CL_APPLY  = {120, 202, 110, 34, "Change"};
static const ui_button BTN_CL_CANCEL = {234, 202, 82, 34, "Back"};

// Stats/level the unit would have in another class (computed on the save, then restored).
static void preview_class(const fe_unit *u, int cls, fe_stats *st, int *level, int *cap)
{
    uint8_t *b = app.save->data + u->off + 0x03;
    uint8_t saved = *b;
    *b = (uint8_t)cls;
    fe_unit_stats(app.save, u, st);
    *cap = fe_unit_level_cap(app.save, u);
    *b = saved;
    int lv = fe_unit_level(app.save, u);
    *level = lv > *cap ? *cap : lv;
}

static void screen_class(u32 down, u32 repeat, const touchPosition *touch)
{
    const fe_unit *u = selected_unit();
    fe_save *s = app.save;
    int cur = fe_unit_class_id(s, u);
    const int NCLS = fe_class_count();

    if (repeat & KEY_UP) app.class_sel = (app.class_sel + NCLS - 1) % NCLS;
    if (repeat & KEY_DOWN) app.class_sel = (app.class_sel + 1) % NCLS;
    if (repeat & (KEY_LEFT | KEY_L)) app.class_sel = app.class_sel >= CLASS_ROWS ? app.class_sel - CLASS_ROWS : 0;
    if (repeat & (KEY_RIGHT | KEY_R))
        app.class_sel = app.class_sel + CLASS_ROWS < NCLS ? app.class_sel + CLASS_ROWS : NCLS - 1;
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_CL_PREV, touch))
            app.class_sel = app.class_scroll >= CLASS_ROWS ? app.class_scroll - CLASS_ROWS : 0;
        if (ui_button_hit(&BTN_CL_NEXT, touch))
            app.class_sel = app.class_scroll + CLASS_ROWS < NCLS ? app.class_scroll + CLASS_ROWS : NCLS - 1;
        if (ui_button_hit(&BTN_CL_APPLY, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_CL_CANCEL, touch)) down |= KEY_B;
        for (int r = 0; r < CLASS_ROWS && app.class_scroll + r < NCLS; r++) {
            ui_button row = {0, 2 + r * 20.0f, BOTTOM_W, 19, NULL};
            if (ui_button_hit(&row, touch)) app.class_sel = app.class_scroll + r;
        }
    }
    if (app.class_sel < app.class_scroll) app.class_scroll = app.class_sel;
    if (app.class_sel >= app.class_scroll + CLASS_ROWS) app.class_scroll = app.class_sel - CLASS_ROWS + 1;
    if (app.class_scroll > NCLS - CLASS_ROWS) app.class_scroll = NCLS - CLASS_ROWS;
    if (app.class_scroll < 0) app.class_scroll = 0;

    const fe_class_info *sel = fe_class(app.class_sel);
    bool other_gender = sel->female != fe_unit_is_female(s, u);

    if ((down & KEY_A) && app.class_sel != cur) {
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        int old_lv = fe_unit_level(s, u);
        fe_unit_set_class(s, u, app.class_sel);
        app.dirty = true;
        LOGI("class edit %s: %s -> %s (Lv %d -> %d)%s%s", name, fe_class_name(cur), sel->name, old_lv,
             fe_unit_level(s, u), other_gender ? " [other gender]" : "", sel->enemy_only ? " [enemy class]" : "");
        toast("Changed to %s", sel->name);
        cur = app.class_sel;
    }
    if (down & KEY_B) { app.st = app.class_return; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    fe_stats now, after;
    bool has_now = fe_unit_stats(s, u, &now);
    int lv_after, cap_after;
    preview_class(u, app.class_sel, &after, &lv_after, &cap_after);
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s - Class", name);
    ui_text_right(TOP_W - 8, 3, 0.55f, CLR_TEXT, "Now: %s", fe_class_name(cur));

    ui_text(12, 28, 0.62f, CLR_ACCENT, "%s", sel->name);
    char tags[96];
    snprintf(tags, sizeof(tags), "%s%s%s", sel->promoted ? "Promoted  " : "Unpromoted  ",
             sel->female ? "Female class" : "Male class", app.class_sel == cur ? "   (current)" : "");
    ui_text(12, 48, 0.45f, CLR_DIM, "%s", tags);
    if (other_gender)
        ui_text(12, 64, 0.45f, CLR_WARN, "Other gender: no matching model in vanilla, animations may glitch.");
    else if (sel->enemy_only)
        ui_text(12, 64, 0.45f, CLR_WARN, "Enemy class: not made for player units, may glitch.");
    if (sel->enemy_only && other_gender)
        ui_text(12, 78, 0.45f, CLR_WARN, "Enemy class too. A backup is made before every save.");

    // Stats now -> after
    ui_text(12, 96, 0.45f, CLR_DIM, "Level %d/%d -> %d/%d", fe_unit_level(s, u), fe_unit_level_cap(s, u), lv_after, cap_after);
    for (int i = 0; i < FE_STAT_COUNT; i++) {
        float cx = 12 + (i % 4) * 96, cy = 114 + (i / 4) * 20;
        int a = has_now ? fe_stat_shown(&now, i) : 0, b = fe_stat_shown(&after, i);
        ui_text(cx, cy, 0.48f, CLR_DIM, "%s", fe_stat_names[i]);
        ui_text(cx + 30, cy, 0.48f, b > a ? CLR_GOOD : (b < a ? CLR_ERR : CLR_TEXT), "%d>%d", a, b);
    }
    ui_text(12, 158, 0.42f, CLR_DIM, "Max: %d/%d/%d/%d/%d/%d/%d/%d", after.cap[0], after.cap[1], after.cap[2],
            after.cap[3], after.cap[4], after.cap[5], after.cap[6], after.cap[7]);
    if (app.toast_frames > 0)
        ui_text(12, 186, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "A: change to this class   L/R: page   B: back");

    ui_bottom();
    for (int r = 0; r < CLASS_ROWS && app.class_scroll + r < NCLS; r++) {
        int c = app.class_scroll + r;
        float y = 2 + r * 20.0f;
        const fe_class_info *ci = fe_class(c);
        bool og = ci->female != fe_unit_is_female(s, u);
        if (c == app.class_sel) ui_rect(0, y - 1, BOTTOM_W, 20, CLR_SEL);
        ui_text(8, y + 1, 0.48f, c == cur ? CLR_ACCENT : CLR_TEXT, "%s%s", ci->name, c == cur ? "  *" : "");
        if (ci->enemy_only) ui_text_right(BOTTOM_W - 6, y + 2, 0.42f, CLR_WARN, "enemy");
        else if (og) ui_text_right(BOTTOM_W - 6, y + 2, 0.42f, CLR_WARN, "other gender");
        else if (c >= FE_CLASS_COUNT) ui_text_right(BOTTOM_W - 6, y + 2, 0.42f, CLR_GOOD, "mod");
        else if (ci->promoted) ui_text_right(BOTTOM_W - 6, y + 2, 0.42f, CLR_DIM, "promoted");
    }
    ui_button_draw(&BTN_CL_PREV, app.class_scroll > 0);
    ui_button_draw(&BTN_CL_NEXT, app.class_scroll + CLASS_ROWS < NCLS);
    ui_button_draw(&BTN_CL_APPLY, app.class_sel != cur);
    ui_button_draw(&BTN_CL_CANCEL, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Supports screen
//---------------------------------------------------------------------------
#define SUP_ROWS 9

static const char *const SUPPORT_TYPE_NAMES[4] = {"Friendship (C-A)", "Romance, slow", "Romance, medium", "Romance, fast"};
static const ui_button BTN_SP_PREV = {4, 202, 54, 34, "<<"};
static const ui_button BTN_SP_NEXT = {62, 202, 54, 34, ">>"};
static const ui_button BTN_SP_DONE = {234, 202, 82, 34, "Done"};

static const char *char_name(int id)
{
    static char buf[24];
    if (fe_char(id)) return fe_char(id)->name;
    snprintf(buf, sizeof(buf), "Character #%d", id);
    return buf;
}

// Support rows for character `id`: listed partners that can reach a rank (romfs mods can leave
// empty or placeholder slots). out[] receives slot indices.
static int support_rows(int id, int *out)
{
    const fe_support_list *l = fe_char_supports(id);
    int n = 0;
    for (int k = 0; l && k < l->count; k++)
        if (l->partners[k].partner != FE_SUPPORT_EMPTY && l->partners[k].type <= 3) out[n++] = k;
    return n;
}

// Applies a new point value for partner slot k of the selected unit (both sides).
static void support_apply(int k, int value)
{
    int ui = app.visible[app.unit_sel];
    fe_save *s = app.save;
    uint16_t id = fe_unit_char_id(s, &s->units[ui]);
    const fe_support_def *d = &fe_char_supports(id)->partners[k];
    int old = fe_support_get(s, &s->units[ui], d->partner);
    if (value == old)
        return;
    char err[160], name[48], rank[16];
    fe_unit_name(s, &s->units[ui], name, sizeof(name));
    uint32_t size_before = s->size;
    if (!fe_support_set(s, ui, d->partner, value, err, sizeof(err))) {
        LOGE("support edit %s - %s failed: %s", name, char_name(d->partner), err);
        toast("Could not change support: %s", err);
        return;
    }
    app.dirty = true;
    fe_support_rank_name(d->type, value, rank, sizeof(rank));
    LOGI("support edit %s - %s: %d -> %d (%s)%s", name, char_name(d->partner), old, value, rank,
         s->size != size_before ? " [save grew]" : "");
    toast("%s: %s (%d points)", char_name(d->partner), rank, value);
}

static void screen_supports(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    const fe_unit *u = selected_unit();
    uint16_t id = fe_unit_char_id(s, u);
    int rows[FE_MAX_SUPPORTS];
    int count = support_rows(id, rows);
    const fe_support_list *sl = fe_char_supports(id);

    if (count > 0) {
        if (repeat & KEY_UP) app.sup_sel = (app.sup_sel + count - 1) % count;
        if (repeat & KEY_DOWN) app.sup_sel = (app.sup_sel + 1) % count;
        if (repeat & KEY_L) app.sup_sel = app.sup_sel >= SUP_ROWS ? app.sup_sel - SUP_ROWS : 0;
        if (repeat & KEY_R) app.sup_sel = app.sup_sel + SUP_ROWS < count ? app.sup_sel + SUP_ROWS : count - 1;
    }
    if (app.sup_sel >= count) app.sup_sel = count ? count - 1 : 0;

    int act_k = -1, act_dir = 0;  // -1/+1 jump, 2 = type a value
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_SP_PREV, touch)) app.sup_sel = app.sup_scroll >= SUP_ROWS ? app.sup_scroll - SUP_ROWS : 0;
        if (ui_button_hit(&BTN_SP_NEXT, touch)) app.sup_sel = app.sup_scroll + SUP_ROWS < count ? app.sup_scroll + SUP_ROWS : count - 1;
        if (ui_button_hit(&BTN_SP_DONE, touch)) down |= KEY_B;
        for (int r = 0; r < SUP_ROWS && app.sup_scroll + r < count; r++) {
            float y = 2 + r * 22.0f;
            ui_button name = {0, y, 200, 21, NULL}, minus = {204, y, 54, 21, NULL}, plus = {262, y, 54, 21, NULL};
            int k = app.sup_scroll + r;
            if (ui_button_hit(&name, touch)) { app.sup_sel = k; act_k = k; act_dir = 2; }
            if (ui_button_hit(&minus, touch)) { app.sup_sel = k; act_k = k; act_dir = -1; }
            if (ui_button_hit(&plus, touch)) { app.sup_sel = k; act_k = k; act_dir = +1; }
        }
    }
    if (count > 0) {
        if (repeat & KEY_LEFT) { act_k = app.sup_sel; act_dir = -1; }
        if (repeat & KEY_RIGHT) { act_k = app.sup_sel; act_dir = +1; }
        if (down & KEY_Y) { act_k = app.sup_sel; act_dir = 2; }
    }
    if (app.sup_sel < app.sup_scroll) app.sup_scroll = app.sup_sel;
    if (app.sup_sel >= app.sup_scroll + SUP_ROWS) app.sup_scroll = app.sup_sel - SUP_ROWS + 1;

    if (act_k >= 0) {
        const fe_support_def *d = &sl->partners[rows[act_k]];
        if (fe_find_unit(s, d->partner) < 0) {
            toast("%s isn't recruited in this save yet.", char_name(d->partner));
        } else {
            int cur = fe_support_get(s, u, d->partner), want = cur;
            if (act_dir == 1) want = fe_support_next_step(d->type, cur);
            if (act_dir == -1) want = fe_support_prev_step(d->type, cur);
            if (act_dir == 2 && !ask_number("Support points", cur, 3, &want)) want = cur;
            if (want < 0) {
                toast("Already at the highest rank for this pair.");
            } else {
                int ui_idx = app.visible[app.unit_sel];
                int sr = d->type >= 1 ? fe_support_prev_ready(d->type, 255) : -1;  // S ready (romance only)
                bool unmarry = sr > 0 && want == sr && fe_support_s_would_unmarry(s, ui_idx, d->partner);
                bool extra_s = sr > 0 && want > sr && fe_support_s_would_unmarry(s, ui_idx, d->partner);
                support_apply(rows[act_k], want);
                if (unmarry)
                    toast("Warning: watching this S in game undoes an existing marriage. + again = S directly.");
                else if (extra_s)
                    toast("Extra S support set directly (no conversation). Multiple S is experimental.");
            }
        }
        u = selected_unit();  // the save may have been re-laid out
    }
    if (down & KEY_B) { app.st = ST_UNITS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s - Supports", name);
    if (count == 0) {
        ui_text_wrap(12, 40, 0.5f, CLR_DIM, TOP_W - 24, "This unit has no support partners in the game data.");
    } else {
        const fe_support_def *d = &sl->partners[rows[app.sup_sel]];
        int v = fe_support_get(s, u, d->partner);
        bool present = fe_find_unit(s, d->partner) >= 0;
        char rank[16];
        fe_support_rank_name(d->type, v, rank, sizeof(rank));
        ui_text(12, 30, 0.62f, CLR_ACCENT, "%s", char_name(d->partner));
        ui_text(12, 52, 0.48f, CLR_DIM, "%s", SUPPORT_TYPE_NAMES[d->type < 4 ? d->type : 0]);
        int spouse = fe_support_spouse(s, app.visible[app.unit_sel], -1);
        if (spouse >= 0)
            ui_text_right(TOP_W - 12, 52, 0.48f, CLR_GOOD, "%s is married to %s", name, char_name(spouse));
        ui_text(12, 72, 0.55f, present ? CLR_TEXT : CLR_DIM, present ? "Rank: %s   (%d points)" : "Not recruited yet",
                rank, v);
        int nr = fe_support_next_ready(d->type, v);
        if (present && nr >= 0) {
            char nrank[16];
            fe_support_rank_name(d->type, nr, nrank, sizeof(nrank));
            ui_text(12, 92, 0.45f, CLR_DIM, "+ sets %d points: %s", nr, nrank);
        }
        ui_text_wrap(12, 116, 0.45f, CLR_DIM, TOP_W - 24,
                     "\"ready\" = the conversation shows up with \"!\" in the game's Support menu; watching it raises "
                     "the rank. A plain rank (C/B/A/S) is set directly, no conversation needed. S directly lets one "
                     "unit have several S supports (experimental).");
    }
    if (app.toast_frames > 0)
        ui_text(12, 190, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "-/+: step through ranks (ready / reached)   Y: type points   B: back");

    ui_bottom();
    for (int r = 0; r < SUP_ROWS && app.sup_scroll + r < count; r++) {
        int k = app.sup_scroll + r;
        const fe_support_def *d = &sl->partners[rows[k]];
        float y = 2 + r * 22.0f;
        bool present = fe_find_unit(s, d->partner) >= 0;
        int v = fe_support_get(s, u, d->partner);
        char rank[16];
        fe_support_rank_name(d->type, v, rank, sizeof(rank));
        if (k == app.sup_sel) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        ui_text(6, y + 2, 0.48f, present ? CLR_TEXT : CLR_DIM, "%s%s", char_name(d->partner), d->type ? " *" : "");
        ui_text_right(198, y + 2, 0.48f, strstr(rank, "ready") ? CLR_ACCENT : (present ? CLR_GOOD : CLR_DIM), "%s", rank);
        ui_button minus = {204, y, 54, 20, "-"}, plus = {262, y, 54, 20, "+"};
        ui_button_draw(&minus, present);
        ui_button_draw(&plus, present);
    }
    ui_text(122, 214, 0.42f, CLR_DIM, "* romance");
    ui_button_draw(&BTN_SP_PREV, app.sup_scroll > 0);
    ui_button_draw(&BTN_SP_NEXT, app.sup_scroll + SUP_ROWS < count);
    ui_button_draw(&BTN_SP_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Army screen: army-wide values (gold, renown)
//---------------------------------------------------------------------------
#define ARMY_ROWS 6
static const char *const DIFF_NAMES[4] = {"Normal", "Hard", "Lunatic", "Lunatic+"};

static int diff_level(void)  // 0..3, Lunatic+ = 3
{
    int d = fe_difficulty(app.save);
    return d == 2 && fe_is_lunatic_plus(app.save) ? 3 : d > 2 ? 2 : d;
}

static void cycle_difficulty(int dir)
{
    int old = diff_level(), now = (old + 4 + dir) % 4;
    fe_set_difficulty(app.save, now >= 2 ? 2 : now);
    fe_set_lunatic_plus(app.save, now == 3);
    app.dirty = true;
    LOGI("difficulty: %s -> %s", DIFF_NAMES[old], DIFF_NAMES[now]);
}

static void toggle_mode(void)
{
    bool c = !fe_is_casual(app.save);
    fe_set_casual(app.save, c);
    app.dirty = true;
    LOGI("mode: %s -> %s", c ? "Classic" : "Casual", c ? "Casual" : "Classic");
}
static const ui_button BTN_AR_DONE   = {234, 202, 82, 34, "Done"};

static void screen_barracks_open(void);

static ui_button army_cell(int i)
{
    ui_button b = {6 + (i % 2) * 156.0f, 6 + (i / 2) * 64.0f, 152, 60, NULL};
    return b;
}

static void screen_army(u32 down, u32 repeat, const touchPosition *touch)
{
    int before = app.army_sel;
    if (repeat & KEY_UP) app.army_sel = (app.army_sel + ARMY_ROWS - 2) % ARMY_ROWS;
    if (repeat & KEY_DOWN) app.army_sel = (app.army_sel + 2) % ARMY_ROWS;
    if (repeat & (KEY_LEFT | KEY_RIGHT)) app.army_sel ^= 1;
    int edit = -1;
    if (down & KEY_TOUCH) {
        for (int c = 0; c < ARMY_ROWS; c++) {
            ui_button b = army_cell(c);
            if (ui_button_hit(&b, touch)) { app.army_sel = c; edit = c; }
        }
        if (ui_button_hit(&BTN_AR_DONE, touch)) down |= KEY_B;
    }
    if (app.army_sel != before) app.army_armed = 0;
    if (down & KEY_A) edit = app.army_sel;
    if (down & KEY_X) { app.conv_sel = 0; app.conv_scroll = 0; app.conv_filter = -1; app.list_count = 0; app.st = ST_CONVOY; return; }
    if (edit == 0) edit_gold();
    if (edit == 1) edit_renown();
    if (edit == 2) cycle_difficulty(+1);
    if (edit == 3) toggle_mode();
    if (edit == 4) { screen_barracks_open(); return; }
    if (edit == 5) {
        int n = fe_renown_claimed(app.save);
        if (!n) toast("No rewards are marked claimed");
        else if (!app.army_armed) { app.army_armed = 1; toast("Press again to make %d rewards claimable again", n); }
        else {
            fe_renown_reset_claims(app.save);
            app.dirty = true;
            app.army_armed = 0;
            LOGI("renown: %d claimed rewards reset", n);
            toast("%d Renown rewards can be claimed again", n);
        }
    }
    if (down & KEY_B) { app.army_armed = 0; app.st = ST_UNITS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    char gold[16], renown[16], claimed[24];
    fmt_gold(gold, sizeof(gold), fe_get_gold(app.save));
    fmt_gold(renown, sizeof(renown), fe_get_renown(app.save));
    int nclaimed = fe_renown_claimed(app.save);
    snprintf(claimed, sizeof(claimed), "%d claimed", nclaimed);
    int pending = 0;
    for (int e = 0; e < FE_BARRACKS_SLOTS; e++) {
        fe_barracks_event ev;
        fe_barracks_get(app.save, e, &ev);
        pending += ev.type != FE_BEV_NONE;
    }
    char bar[24];
    snprintf(bar, sizeof(bar), "%d event%s set", pending, pending == 1 ? "" : "s");
    const char *labels[ARMY_ROWS] = {"Gold", "Renown", "Difficulty", "Mode", "Barracks", "Renown rewards"};
    const char *values[ARMY_ROWS] = {gold, renown, DIFF_NAMES[diff_level()], fe_is_casual(app.save) ? "Casual" : "Classic",
                                     bar, claimed};
    static const char *const info[ARMY_ROWS] = {
        "Army funds (max 999,999). Tap to type.",
        "The army score that unlocks rewards in the Renown menu (max 99,999). Tap to type.",
        "Normal, Hard, Lunatic or Lunatic+. Applies from the next battle. Tap to change.",
        "Classic or Casual (fallen units return after each battle). Tap to change.",
        "Set the events waiting in the Barracks: stat boosts, exp, found items, conversations, "
        "birthdays. Experimental.",
        "Clears the claimed marks so every Renown reward you have reached can be claimed again. "
        "Tap twice."};

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Slot %d - Army%s", app.slot + 1, app.dirty ? "  * unsaved" : "");
    for (int r = 0; r < ARMY_ROWS; r++) {
        float y = 28 + r * 22;
        if (r == app.army_sel) ui_rect(8, y - 2, TOP_W - 16, 21, CLR_SEL);
        ui_text(20, y, 0.55f, CLR_TEXT, "%s", labels[r]);
        ui_text_right(TOP_W - 20, y, 0.55f, CLR_ACCENT, "%s", values[r]);
    }
    ui_text_wrap(12, 166, 0.44f, CLR_DIM, TOP_W - 24, info[app.army_sel]);
    if (app.toast_frames > 0) ui_text(12, 204, 0.46f, app.army_armed ? CLR_WARN : CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "D-pad: select   A: change / open   X: convoy   B: back");

    ui_bottom();
    for (int c = 0; c < ARMY_ROWS; c++) {
        ui_button b = army_cell(c);
        ui_rect(b.x, b.y, b.w, b.h, c == app.army_sel ? (c == 5 && app.army_armed ? CLR_ACCENT : CLR_SEL) : CLR_BTN);
        ui_text(b.x + 10, b.y + 8, 0.55f, CLR_TEXT, "%s", labels[c]);
        ui_text(b.x + 10, b.y + 32, 0.48f, CLR_ACCENT, "%s", c == 5 && app.army_armed ? "Tap again" : values[c]);
    }
    ui_button_draw(&BTN_AR_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Item lists (shared by the picker and the convoy)
//---------------------------------------------------------------------------
#define ITEM_ROWS 9
static const char *item_name_of(int id)
{
    return fe_item(id) ? fe_item(id)->name : "?";
}

static const char *const FILTER_NAMES[12] = {"All", "Sword", "Lance", "Axe", "Bow", "Tome", "Staff",
                                             "Dragonstone", "Beaststone", "Claws", "Breath", "Item"};

static int item_type_of(int id)
{
    int base = fe_item_base(app.save, id);
    return fe_item(base) ? fe_item(base)->type : 10;
}

// Builds app.list_ids: every regular item plus existing forges, filtered by type.
static void build_item_list(int filter)
{
    app.list_count = 0;
    int forge_base = app.save->convoy_count - FE_FORGE_MAX;
    for (int id = 1; id < fe_item_count() && id < forge_base && app.list_count < FE_MAX_ITEMS; id++)
        if (filter < 0 || fe_item(id)->type == filter)
            app.list_ids[app.list_count++] = id;
    for (int f = 0; f < app.save->forge_count; f++) {
        int id = forge_base + app.save->data[app.save->refi_off + 7 + f * 0x2C];
        if (filter < 0 || item_type_of(id) == filter)
            app.list_ids[app.list_count++] = id;
    }
}

static int list_index_of(int id)
{
    for (int i = 0; i < app.list_count; i++)
        if (app.list_ids[i] == id) return i;
    return 0;
}

static void list_nav(u32 repeat, int *sel, int *scroll, int rows)
{
    int n = app.list_count;
    if (n <= 0) { *sel = 0; *scroll = 0; return; }
    if (repeat & KEY_UP) *sel = (*sel + n - 1) % n;
    if (repeat & KEY_DOWN) *sel = (*sel + 1) % n;
    if (repeat & KEY_L) *sel = *sel >= rows ? *sel - rows : 0;
    if (repeat & KEY_R) *sel = *sel + rows < n ? *sel + rows : n - 1;
    if (*sel >= n) *sel = n - 1;
    if (*sel < *scroll) *scroll = *sel;
    if (*sel >= *scroll + rows) *scroll = *sel - rows + 1;
}

//---------------------------------------------------------------------------
// Inventory screen (per unit)
//---------------------------------------------------------------------------
static const ui_button BTN_IV_PICK  = {4, 202, 62, 34, "Change"};
static const ui_button BTN_IV_EQUIP = {68, 202, 56, 34, "Equip"};
static const ui_button BTN_IV_DEL   = {126, 202, 62, 34, "Remove"};
static const ui_button BTN_IV_FORGE = {190, 202, 60, 34, "Forge"};
static const ui_button BTN_IV_DONE  = {252, 202, 64, 34, "Done"};

static void inv_log(const char *what, int slot)
{
    char name[48], item[32];
    const fe_unit *u = selected_unit();
    fe_inv_item it;
    fe_unit_name(app.save, u, name, sizeof(name));
    fe_inv_get(app.save, u, slot, &it);
    fe_item_name(app.save, it.id, item, sizeof(item));
    LOGI("item edit %s: %s slot %d = %s (%d uses)", name, what, slot + 1, item, it.uses);
}

static void screen_items(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    const fe_unit *u = selected_unit();
    fe_inv_item it[FE_INV_SLOTS];
    int used = 0;
    for (int k = 0; k < FE_INV_SLOTS; k++) { fe_inv_get(s, u, k, &it[k]); if (it[k].id) used = k + 1; }
    int rows = used < FE_INV_SLOTS ? used + 1 : FE_INV_SLOTS;  // filled slots + one empty to add into

    if (repeat & KEY_UP) app.inv_sel = (app.inv_sel + rows - 1) % rows;
    if (repeat & KEY_DOWN) app.inv_sel = (app.inv_sel + 1) % rows;
    if (app.inv_sel >= rows) app.inv_sel = rows - 1;

    int du = 0;
    if (down & KEY_TOUCH) {
        for (int k = 0; k < rows; k++) {
            ui_button row = {0, 4 + k * 34.0f, 196, 32, NULL};
            ui_button minus = {200, 4 + k * 34.0f, 56, 32, NULL}, plus = {260, 4 + k * 34.0f, 56, 32, NULL};
            if (ui_button_hit(&row, touch)) { if (app.inv_sel == k) down |= KEY_A; app.inv_sel = k; }
            if (ui_button_hit(&minus, touch)) { app.inv_sel = k; du = -1; }
            if (ui_button_hit(&plus, touch)) { app.inv_sel = k; du = +1; }
        }
        if (ui_button_hit(&BTN_IV_PICK, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_IV_EQUIP, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_IV_DEL, touch)) down |= KEY_SELECT;
        if (ui_button_hit(&BTN_IV_FORGE, touch)) down |= KEY_START;
        if (ui_button_hit(&BTN_IV_DONE, touch)) down |= KEY_B;
    }
    if ((down & KEY_START) && it[app.inv_sel].id) { open_forge(0, app.inv_sel, it[app.inv_sel].id, ST_ITEMS); return; }
    if (repeat & KEY_LEFT) du = -1;
    if (repeat & KEY_RIGHT) du = +1;
    if (repeat & KEY_L) du = -10;
    if (repeat & KEY_R) du = +10;

    fe_inv_item *cur = &it[app.inv_sel];
    if (du && cur->id) {
        fe_inv_set(s, u, app.inv_sel, cur->id, cur->uses + du);
        app.dirty = true;
        inv_log("uses", app.inv_sel);
    }
    if ((down & KEY_Y) && cur->id) {
        int want;
        if (ask_number("Uses (0-255)", cur->uses, 3, &want)) {
            fe_inv_set(s, u, app.inv_sel, cur->id, want);
            app.dirty = true;
            inv_log("uses", app.inv_sel);
        }
    }
    if ((down & KEY_X) && cur->id) {
        fe_inv_equip(s, u, app.inv_sel);
        app.dirty = true;
        inv_log("equip", 0);
        app.inv_sel = 0;
        toast("Equipped (moved to the top, like the game does)");
    }
    if ((down & KEY_SELECT) && cur->id) {
        inv_log("remove", app.inv_sel);
        fe_inv_set(s, u, app.inv_sel, 0, 0);
        app.dirty = true;
    }
    if (down & KEY_A) {
        app.pick_filter = -1;
        build_item_list(app.pick_filter);
        app.pick_sel = cur->id ? list_index_of(cur->id) : 0;
        app.pick_scroll = app.pick_sel;
        app.st = ST_ITEM_PICK;
        return;
    }
    if (down & KEY_B) { app.st = ST_UNITS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    for (int k = 0; k < FE_INV_SLOTS; k++) fe_inv_get(s, u, k, &it[k]);
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s - Items", name);
    for (int k = 0; k < FE_INV_SLOTS; k++) {
        float y = 32 + k * 24;
        if (k == app.inv_sel) ui_rect(8, y - 3, TOP_W - 16, 22, CLR_SEL);
        char nm[32];
        if (it[k].id) fe_item_name(s, it[k].id, nm, sizeof(nm));
        int mx = fe_item_max_uses(s, it[k].id);
        ui_text(16, y, 0.55f, it[k].id ? CLR_TEXT : CLR_DIM, "%d  %s", k + 1, it[k].id ? nm : "(empty)");
        if (it[k].id) {
            if (mx) ui_text_right(330, y, 0.55f, it[k].uses > mx ? CLR_ACCENT : CLR_TEXT, "%d/%d", it[k].uses, mx);
            else ui_text_right(330, y, 0.55f, CLR_DIM, "--");
            if (it[k].flags & FE_ITEM_EQUIPPED) ui_text(344, y, 0.5f, CLR_GOOD, "E");
            if (it[k].flags & 0x20) ui_text(362, y, 0.5f, CLR_WARN, "drop");
        }
    }
    if (app.toast_frames > 0) ui_text(12, 160, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 190, 0.42f, CLR_DIM, "Uses above the normal max are allowed (yellow). -- = unbreakable");
    ui_text(8, 206, 0.42f, CLR_DIM, "A: change item  X: equip  SELECT: remove  Left/Right L/R: uses");
    ui_text(8, 222, 0.42f, CLR_DIM, "Y: type uses   START: forge   B: back");

    ui_bottom();
    for (int k = 0; k < rows; k++) {
        float y = 4 + k * 34.0f;
        if (k == app.inv_sel) ui_rect(0, y - 1, BOTTOM_W, 34, CLR_SEL);
        char nm[32];
        if (it[k].id) fe_item_name(s, it[k].id, nm, sizeof(nm));
        ui_text(8, y + 8, 0.5f, it[k].id ? CLR_TEXT : CLR_DIM, "%s", it[k].id ? nm : "+ add item");
        if (it[k].id) {
            ui_button minus = {200, y, 56, 32, "-"}, plus = {260, y, 56, 32, "+"};
            ui_button_draw(&minus, true);
            ui_button_draw(&plus, true);
        }
    }
    ui_button_draw(&BTN_IV_PICK, true);
    ui_button_draw(&BTN_IV_EQUIP, it[app.inv_sel].id != 0);
    ui_button_draw(&BTN_IV_DEL, it[app.inv_sel].id != 0);
    ui_button_draw(&BTN_IV_FORGE, it[app.inv_sel].id != 0);
    ui_button_draw(&BTN_IV_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Item picker
//---------------------------------------------------------------------------
static const ui_button BTN_PK_PREV   = {4, 202, 54, 34, "<<"};
static const ui_button BTN_PK_NEXT   = {62, 202, 54, 34, ">>"};
static const ui_button BTN_PK_FILTER = {120, 202, 110, 34, "Type"};
static const ui_button BTN_PK_CANCEL = {234, 202, 82, 34, "Cancel"};

static void screen_item_pick(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    const fe_unit *u = selected_unit();
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_PK_PREV, touch)) repeat |= KEY_L;
        if (ui_button_hit(&BTN_PK_NEXT, touch)) repeat |= KEY_R;
        if (ui_button_hit(&BTN_PK_FILTER, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_PK_CANCEL, touch)) down |= KEY_B;
        for (int r = 0; r < ITEM_ROWS && app.pick_scroll + r < app.list_count; r++) {
            ui_button row = {0, 2 + r * 22.0f, BOTTOM_W, 21, NULL};
            if (ui_button_hit(&row, touch)) {
                if (app.pick_sel == app.pick_scroll + r) down |= KEY_A;
                app.pick_sel = app.pick_scroll + r;
            }
        }
    }
    if (down & KEY_X) {
        app.pick_filter = app.pick_filter >= 10 ? -1 : app.pick_filter + 1;
        build_item_list(app.pick_filter);
        app.pick_sel = app.pick_scroll = 0;
    }
    list_nav(repeat, &app.pick_sel, &app.pick_scroll, ITEM_ROWS);

    if ((down & KEY_A) && app.list_count > 0) {
        int id = app.list_ids[app.pick_sel];
        int mx = fe_item_max_uses(s, id);
        fe_inv_set(s, u, app.inv_sel, id, mx);  // unbreakable items store 0 uses, like the game
        app.dirty = true;
        inv_log("set", app.inv_sel);
        app.st = ST_ITEMS;
        return;
    }
    if (down & KEY_B) { app.st = ST_ITEMS; return; }

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Choose an item for slot %d", app.inv_sel + 1);
    ui_text_right(TOP_W - 8, 3, 0.5f, CLR_TEXT, "Type: %s", FILTER_NAMES[app.pick_filter + 1]);
    if (app.list_count > 0) {
        int id = app.list_ids[app.pick_sel];
        char nm[32];
        fe_item_name(s, id, nm, sizeof(nm));
        int mx = fe_item_max_uses(s, id);
        ui_text(12, 40, 0.65f, CLR_ACCENT, "%s", nm);
        ui_text(12, 66, 0.5f, CLR_DIM, "%s   %s", fe_item_type_names[item_type_of(id)],
                mx ? "" : "unbreakable");
        if (mx) ui_text(12, 86, 0.5f, CLR_DIM, "Max uses: %d (set full when chosen)", mx);
        if (fe_forge_of_item(s, id) >= 0) ui_text(12, 106, 0.45f, CLR_DIM, "* = forged weapon from this save");
        else if (id >= FE_ITEM_COUNT) ui_text(12, 106, 0.45f, CLR_GOOD, "Item from %s", femod_active() ? femod_active()->label : "a mod");
    }
    ui_text(8, 222, 0.42f, CLR_DIM, "A: choose   X: weapon type filter   L/R: page   B: cancel");

    ui_bottom();
    for (int r = 0; r < ITEM_ROWS && app.pick_scroll + r < app.list_count; r++) {
        int i = app.pick_scroll + r, id = app.list_ids[i];
        float y = 2 + r * 22.0f;
        char nm[32];
        fe_item_name(s, id, nm, sizeof(nm));
        if (i == app.pick_sel) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        ui_text(8, y + 2, 0.48f, CLR_TEXT, "%s", nm);
        ui_text_right(BOTTOM_W - 6, y + 3, 0.42f, CLR_DIM, "%s", fe_item_type_names[item_type_of(id)]);
    }
    ui_button_draw(&BTN_PK_PREV, app.pick_scroll > 0);
    ui_button_draw(&BTN_PK_NEXT, app.pick_scroll + ITEM_ROWS < app.list_count);
    char fl[24];
    snprintf(fl, sizeof(fl), "Type: %s", FILTER_NAMES[app.pick_filter + 1]);
    ui_button f = BTN_PK_FILTER;
    f.label = fl;
    ui_button_draw(&f, true);
    ui_button_draw(&BTN_PK_CANCEL, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Convoy
//---------------------------------------------------------------------------
static const ui_button BTN_CV_PREV   = {4, 202, 34, 34, "<<"};
static const ui_button BTN_CV_NEXT   = {40, 202, 34, 34, ">>"};
static const ui_button BTN_CV_FILTER = {76, 202, 74, 34, "Type"};
static const ui_button BTN_CV_PLUS10 = {152, 202, 38, 34, "+10"};
static const ui_button BTN_CV_MAX    = {192, 202, 38, 34, "Max"};
static const ui_button BTN_CV_FORGE  = {232, 202, 44, 34, "Forge"};
static const ui_button BTN_CV_DONE   = {278, 202, 38, 34, "Done"};
#define CONVOY_MAX_EACH 99

static void convoy_change(int id, int count)
{
    char nm[32];
    fe_item_name(app.save, id, nm, sizeof(nm));
    int old = fe_convoy_count_items(app.save, id);
    fe_convoy_set_items(app.save, id, count);
    int now = fe_convoy_count_items(app.save, id);
    if (now != old) {
        app.dirty = true;
        LOGI("convoy edit: %s %d -> %d (%d uses)", nm, old, now, fe_convoy_get(app.save, id));
    }
}

static void screen_convoy(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    if (app.list_count == 0 || app.conv_sel >= app.list_count) build_item_list(app.conv_filter);
    int dc = 0, row_hit = -1;
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_CV_PREV, touch)) repeat |= KEY_L;
        if (ui_button_hit(&BTN_CV_NEXT, touch)) repeat |= KEY_R;
        if (ui_button_hit(&BTN_CV_FILTER, touch)) down |= KEY_SELECT;
        if (ui_button_hit(&BTN_CV_PLUS10, touch)) down |= KEY_START;
        if (ui_button_hit(&BTN_CV_FORGE, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_CV_MAX, touch) && app.list_count) {
            for (int i = 0; i < app.list_count; i++) convoy_change(app.list_ids[i], CONVOY_MAX_EACH);
            toast("All %d listed items set to %d", app.list_count, CONVOY_MAX_EACH);
        }
        if (ui_button_hit(&BTN_CV_DONE, touch)) down |= KEY_B;
        for (int r = 0; r < ITEM_ROWS && app.conv_scroll + r < app.list_count; r++) {
            float y = 2 + r * 22.0f;
            ui_button name = {0, y, 150, 21, NULL}, m10 = {152, y, 40, 21, NULL}, minus = {194, y, 40, 21, NULL},
                      plus = {236, y, 40, 21, NULL}, p10 = {278, y, 40, 21, NULL};
            if (ui_button_hit(&name, touch)) { app.conv_sel = app.conv_scroll + r; row_hit = 1; }
            if (ui_button_hit(&m10, touch)) { app.conv_sel = app.conv_scroll + r; dc = -10; }
            if (ui_button_hit(&minus, touch)) { app.conv_sel = app.conv_scroll + r; dc = -1; }
            if (ui_button_hit(&plus, touch)) { app.conv_sel = app.conv_scroll + r; dc = +1; }
            if (ui_button_hit(&p10, touch)) { app.conv_sel = app.conv_scroll + r; dc = +10; }
        }
    }
    if (down & KEY_SELECT) {
        app.conv_filter = app.conv_filter >= 10 ? -1 : app.conv_filter + 1;
        build_item_list(app.conv_filter);
        app.conv_sel = app.conv_scroll = 0;
    }
    list_nav(repeat, &app.conv_sel, &app.conv_scroll, ITEM_ROWS);
    if (repeat & KEY_LEFT) dc = -1;
    if (repeat & KEY_RIGHT) dc = +1;

    int id = app.list_count ? app.list_ids[app.conv_sel] : 0;
    if (dc && id) {
        int want = fe_convoy_count_items(s, id) + dc;
        if (dc > 1 && want > CONVOY_MAX_EACH) want = CONVOY_MAX_EACH;
        convoy_change(id, want < 0 ? 0 : want);
    }
    if (((down & KEY_Y) || row_hit > 0) && id) {
        int want;
        if (ask_number("How many in the convoy", fe_convoy_count_items(s, id), 4, &want))
            convoy_change(id, want);
    }
    if ((down & KEY_A) && id) { open_forge(1, 0, id, ST_CONVOY); return; }
    if ((down & KEY_START) && app.list_count) {
        for (int i = 0; i < app.list_count; i++) {
            int c = fe_convoy_count_items(s, app.list_ids[i]) + 10;
            convoy_change(app.list_ids[i], c > CONVOY_MAX_EACH ? CONVOY_MAX_EACH : c);
        }
        toast("+10 of each of the %d listed items (max %d)", app.list_count, CONVOY_MAX_EACH);
    }
    if ((down & KEY_X) && app.list_count) {
        int want;
        if (ask_number("Set EVERY listed item to how many?", 0, 4, &want)) {
            for (int i = 0; i < app.list_count; i++) convoy_change(app.list_ids[i], want);
            toast("All %d listed items set to %d", app.list_count, want);
        }
    }
    if (down & KEY_B) { app.list_count = 0; app.st = ST_UNITS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    int total = 0;
    for (int i = 1; i < s->convoy_count - FE_FORGE_MAX; i++) total += fe_convoy_count_items(s, i);

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Convoy%s", app.dirty ? "  * unsaved" : "");
    ui_text_right(TOP_W - 8, 3, 0.5f, CLR_TEXT, "%d items   Type: %s", total, FILTER_NAMES[app.conv_filter + 1]);
    if (id) {
        char nm[32];
        fe_item_name(s, id, nm, sizeof(nm));
        int mx = fe_item_max_uses(s, id);
        ui_text(12, 36, 0.65f, CLR_ACCENT, "%s", nm);
        ui_text(12, 62, 0.5f, CLR_TEXT, "In convoy: %d   (%d total uses%s)", fe_convoy_count_items(s, id),
                fe_convoy_get(s, id), mx ? "" : ", unbreakable");
        ui_text(12, 82, 0.45f, CLR_DIM, "%s   %d uses each", fe_item_type_names[item_type_of(id)], mx);
    }
    if (app.toast_frames > 0) ui_text(12, 120, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text_wrap(12, 150, 0.42f, CLR_DIM, TOP_W - 24,
                 "The game stores total uses per item, so partly used items merge into the count.");
    ui_text(8, 206, 0.42f, CLR_DIM, "Left/Right or -/+: one   -10/+10: ten   Y or tap name: type count");
    ui_text(8, 222, 0.42f, CLR_DIM, "A: forge  X: set all  START: +10 all  SELECT: type  L/R: page");

    ui_bottom();
    for (int r = 0; r < ITEM_ROWS && app.conv_scroll + r < app.list_count; r++) {
        int i = app.conv_scroll + r, iid = app.list_ids[i];
        float y = 2 + r * 22.0f;
        char nm[32];
        fe_item_name(s, iid, nm, sizeof(nm));
        int c = fe_convoy_count_items(s, iid);
        if (i == app.conv_sel) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        ui_text(6, y + 2, 0.44f, c ? CLR_TEXT : CLR_DIM, "%.16s", nm);
        ui_text_right(148, y + 2, 0.46f, c ? CLR_ACCENT : CLR_DIM, "%d", c);
        ui_button m10 = {152, y, 40, 20, "-10"}, minus = {194, y, 40, 20, "-"}, plus = {236, y, 40, 20, "+"},
                  p10 = {278, y, 40, 20, "+10"};
        ui_button_draw(&m10, true);
        ui_button_draw(&minus, true);
        ui_button_draw(&plus, true);
        ui_button_draw(&p10, true);
    }
    ui_button_draw(&BTN_CV_PREV, app.conv_scroll > 0);
    ui_button_draw(&BTN_CV_NEXT, app.conv_scroll + ITEM_ROWS < app.list_count);
    char fl[24];
    snprintf(fl, sizeof(fl), "%s", FILTER_NAMES[app.conv_filter + 1]);
    ui_button f = BTN_CV_FILTER;
    f.label = fl;
    ui_button_draw(&f, true);
    ui_button_draw(&BTN_CV_PLUS10, app.list_count > 0);
    ui_button_draw(&BTN_CV_MAX, app.list_count > 0);
    ui_button_draw(&BTN_CV_FORGE, app.list_count > 0);
    ui_button_draw(&BTN_CV_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Extras menu
//---------------------------------------------------------------------------
#define EXTRAS_COUNT 6
static const char *const EXTRAS_NAMES[EXTRAS_COUNT] = {"Avatar look", "Support Log", "Add a unit",
                                                       "Army tools", "World map", "Records & time"};
static const char *const EXTRAS_INFO[EXTRAS_COUNT] = {
    "Change the Avatar's name, gender, build, face, hair, hair color, voice and birthday after "
    "creation. Includes the 16 DLC hero faces (e.g. Marth) the game builds on the Avatar model.",
    "Unlock every support conversation in the Support Log and every unit in the Unit Gallery, and "
    "turn on the Game Clear flag that makes those menus appear. Stored in the shared Global file, "
    "so it applies to all slots.",
    "Add any character to the army as a fresh level-1 recruit, e.g. (M) Robin or (F) Robin with the "
    "default look next to your own Avatar, or each Robin's Morgan (parents are filled in from that "
    "Robin and their S spouse).",
    "One-button edits for every player unit: max stats, learn all skills, max weapon ranks, "
    "supports to the next conversation, heal everyone, revive the fallen.",
    "Lock, open or mark beaten any world map location (chapters and paralogues), e.g. reopen a "
    "beaten chapter to play it again, or put Risen battles and merchants on the map. Experimental.",
    "The total play time and each chapter's record from the credits: turns, time and the two "
    "featured units with their classes."};
static const ui_button BTN_EX_BACK = {234, 202, 82, 34, "Back"};

static bool is_logbook_player(int i)
{
    const fe_unit *u = &app.save->units[i];
    return u->log_off >= 0 && (u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE || u->group == FE_GROUP_DEAD);
}

static int first_avatar(void)
{
    for (int i = 0; i < app.save->unit_count; i++) {
        uint16_t id = fe_unit_char_id(app.save, &app.save->units[i]);
        if (is_logbook_player(i) && (id == 0 || id == 1)) return i;
    }
    for (int i = 0; i < app.save->unit_count; i++)
        if (is_logbook_player(i)) return i;
    return -1;
}

static void av_identity_off(const fe_unit *u);
static int logbook_source(int except);

static void screen_extras(u32 down, u32 repeat, const touchPosition *touch)
{
    if (repeat & KEY_UP) app.extras_sel = (app.extras_sel + EXTRAS_COUNT - 2) % EXTRAS_COUNT;
    if (repeat & KEY_DOWN) app.extras_sel = (app.extras_sel + 2) % EXTRAS_COUNT;
    if (repeat & (KEY_LEFT | KEY_RIGHT)) app.extras_sel ^= 1;
    if (down & KEY_TOUCH) {
        for (int i = 0; i < EXTRAS_COUNT; i++) {
            ui_button b = {6 + (i % 2) * 156.0f, 8 + (i / 2) * 64.0f, 152, 58, NULL};
            if (ui_button_hit(&b, touch)) { app.extras_sel = i; down |= KEY_A; }
        }
        if (ui_button_hit(&BTN_EX_BACK, touch)) down |= KEY_B;
    }
    if (down & KEY_A) {
        if (app.extras_sel == 5) {
            app.rec_sel = app.rec_scroll = 0;
            app.toast_frames = 0;
            app.st = ST_RECORDS;
            return;
        }
        if (app.extras_sel == 4) {
            if (!app.save->map_count) {
                show_message("No world map", CLR_WARN, ST_EXTRAS, "This save's world map data was not recognized.");
                return;
            }
            app.map_sel = app.map_scroll = 0;
            app.toast_frames = 0;
            app.st = ST_WORLD_MAP;
            return;
        }
        if (app.extras_sel == 3) {
            app.tools_sel = 0;
            app.tools_armed = -1;
            app.toast_frames = 0;
            app.st = ST_ARMY_TOOLS;
            return;
        }
        if (app.extras_sel == 2) {
            app.add_sel = app.add_scroll = 0;
            app.add_armed = -1;
            app.toast_frames = 0;
            app.st = ST_ADD_UNIT;
            return;
        }
        if (app.extras_sel == 1) {
            if (global_ensure_loaded()) {
                app.global_row = 0;
                app.toast_frames = 0;
                app.st = ST_GLOBAL;
            }
            return;
        }
        if (app.extras_sel == 0) {
            // Avatars added without look data (e.g. by an older version) get it now
            char fixed[160] = "";
            size_t fo = 0;
            for (int i = 0; i < app.save->unit_count; i++) {
                const fe_unit *u = &app.save->units[i];
                uint16_t c = fe_unit_char_id(app.save, u);
                bool player = u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE || u->group == FE_GROUP_DEAD;
                if (!player || (c != 0 && c != 1) || u->log_off >= 0 || u->child_off >= 0) continue;
                int src = logbook_source(i);
                char err[160];
                if (src >= 0 && fe_unit_add_logbook(app.save, i, src, err, sizeof(err))) {
                    app.dirty = true;
                    LOGI("gave %s logbook data (name Robin)", char_name(c));
                    if (fo < sizeof(fixed))
                        fo += (size_t)snprintf(fixed + fo, sizeof(fixed) - fo, "%s%s", fo ? ", " : "", char_name(c));
                }
            }
            if (fixed[0]) build_visible_list();
            app.av_unit = first_avatar();
            if (app.av_unit < 0) {
                show_message("No Avatar", CLR_WARN, ST_EXTRAS, "This slot has no unit with Avatar/logbook data.");
                return;
            }
            app.av_row = 0;
            app.toast_frames = 0;
            av_identity_off(&app.save->units[app.av_unit]);
            app.st = ST_AVATAR;
            if (fixed[0])
                show_message("Avatar look data added", CLR_ACCENT, ST_AVATAR,
                             "Gave look data to: %s. Avatars need it to show a name and portrait. Save to keep.", fixed);
            return;
        }
    }
    if (down & KEY_B) { app.st = ST_UNITS; return; }

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Extras");
    ui_text(12, 36, 0.62f, CLR_ACCENT, "%s", EXTRAS_NAMES[app.extras_sel]);
    ui_text_wrap(12, 64, 0.48f, CLR_TEXT, TOP_W - 24, EXTRAS_INFO[app.extras_sel]);
    ui_text(8, 222, 0.42f, CLR_DIM, "D-pad: select   A: open   B: back");
    ui_bottom();
    for (int i = 0; i < EXTRAS_COUNT; i++) {
        ui_button b = {6 + (i % 2) * 156.0f, 8 + (i / 2) * 64.0f, 152, 58, EXTRAS_NAMES[i]};
        if (i == app.extras_sel) ui_rect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, CLR_SEL);
        ui_button_draw(&b, true);
    }
    ui_button_draw(&BTN_EX_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Avatar look & name
//---------------------------------------------------------------------------
enum { AV_NAME, AV_GENDER, AV_BUILD, AV_FACE, AV_HAIR, AV_COLOR, AV_VOICE, AV_MONTH, AV_DAY, AV_ROWS };
static const char *const AV_LABELS[AV_ROWS] = {"Name", "Gender", "Build", "Face", "Hairstyle",
                                               "Hair color", "Voice", "Birth month", "Birth day"};
static const uint32_t HAIR_PRESETS[] = {0xF6F4EF, 0xF3E9C3, 0xDFC6A9, 0xE8D27A, 0x6B4A2B, 0x2A2A2A,
                                        0xB83A3A, 0xE88FB4, 0x4FA060, 0x325AB4, 0x7A4FA0, 0x9AA3B2};
#define HAIR_PRESET_COUNT (int)(sizeof(HAIR_PRESETS) / sizeof(HAIR_PRESETS[0]))
static const ui_button BTN_AV_PREV = {4, 202, 58, 34, "<Unit"};
static const ui_button BTN_AV_NEXT = {66, 202, 58, 34, "Unit>"};
static const ui_button BTN_AV_PRESET = {128, 202, 102, 34, "Presets"};
static const ui_button BTN_AV_DONE = {234, 202, 82, 34, "Done"};

static bool ask_text(const char *hint, const char *initial, int max_len, char *out, size_t outlen)
{
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, max_len);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, initial);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    return swkbdInputText(&kb, out, outlen) == SWKBD_BUTTON_CONFIRM;
}

// Face as a step index: 0..4 = normal faces, 5..20 = hero faces
static int face_index(int face)
{
    if (face <= 4) return face;
    for (int i = 0; i < FE_HERO_FACE_COUNT; i++)
        if (fe_hero_faces[i] == face) return 5 + i;
    return -1;  // unknown id (kept as is until changed)
}

static void face_label(int face, char *out, size_t outlen)
{
    int i = face_index(face);
    if (i < 0) snprintf(out, outlen, "id %d (unknown)", face);
    else if (i <= 4) snprintf(out, outlen, "Face %d", i + 1);
    else snprintf(out, outlen, "Hero: %s", fe_hero_face_name(face) ? fe_hero_face_name(face) : "?");
}

#define AVATAR_ID_DIR APP_DIR "/avatar_ids"

static void own_id_path(const fe_unit *u, char *out, size_t len)
{
    snprintf(out, len, AVATAR_ID_DIR "/%016llX_slot%d_char%d.bin", (unsigned long long)app.title->title_id,
             app.slot + 1, fe_unit_char_id(app.save, u));
}

// Undoes the removed "Hero identity" experiment: if the unit still carries a hero id and its
// own id was saved to SD, put the own id back. Called when the Avatar screen opens.
static void av_identity_off(const fe_unit *u)
{
    fe_save *s = app.save;
    if (!fe_log_hero_identity(s, u)) return;
    char path[160];
    uint8_t own[FE_LOG_ID_SIZE];
    own_id_path(u, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(own, 1, sizeof(own), f) == sizeof(own);
    if (f) fclose(f);
    if (!ok) { LOGW("unit has a hero id but no saved own id at %s", path); return; }
    fe_log_id_set(s, u, own);
    fe_log_set_hero_flag(s, u, false);
    app.dirty = true;
    LOGI("avatar edit: own identity restored from %s", path);
    toast("Restored this unit's own logbook id (from the removed hero test). Save to keep.");
}

static void av_log(const fe_unit *u, const char *what, int value)
{
    char name[48];
    fe_unit_name(app.save, u, name, sizeof(name));
    LOGI("avatar edit %s: %s = %d", name, what, value);
}

static void av_step(const fe_unit *u, int row, int d)
{
    fe_save *s = app.save;
    switch (row) {
    case AV_GENDER: fe_look_set(s, u, FE_LOOK_GENDER, fe_look_get(s, u, FE_LOOK_GENDER) ? 0 : 1); break;
    case AV_BUILD: fe_look_set(s, u, FE_LOOK_BUILD, (fe_look_get(s, u, FE_LOOK_BUILD) + 3 + d) % 3); break;
    case AV_HAIR: fe_look_set(s, u, FE_LOOK_HAIR, (fe_look_get(s, u, FE_LOOK_HAIR) + 5 + d) % 5); break;
    case AV_VOICE: fe_look_set(s, u, FE_LOOK_VOICE, (fe_look_get(s, u, FE_LOOK_VOICE) + 5 + d) % 5); break;
    case AV_MONTH: fe_look_set(s, u, FE_LOOK_MONTH, (fe_look_get(s, u, FE_LOOK_MONTH) + 12 - 1 + d) % 12 + 1); break;
    case AV_DAY: fe_look_set(s, u, FE_LOOK_DAY, (fe_look_get(s, u, FE_LOOK_DAY) + 31 - 1 + d) % 31 + 1); break;
    case AV_FACE: {
        int i = face_index(fe_look_get(s, u, FE_LOOK_FACE));
        int n = 5 + FE_HERO_FACE_COUNT;
        i = i < 0 ? 0 : (i + n + d) % n;
        fe_look_set(s, u, FE_LOOK_FACE, i <= 4 ? i : fe_hero_faces[i - 5]);
        break;
    }
    case AV_COLOR: {
        uint32_t cur = fe_look_color(s, u);
        int k = 0;
        for (int i = 0; i < HAIR_PRESET_COUNT; i++) if (HAIR_PRESETS[i] == cur) k = i;
        k = (k + HAIR_PRESET_COUNT + d) % HAIR_PRESET_COUNT;
        fe_look_set_color(s, u, HAIR_PRESETS[k]);
        break;
    }
    default: return;
    }
    app.dirty = true;
    av_log(u, AV_LABELS[row], row == AV_COLOR ? (int)fe_look_color(s, u) : fe_look_get(s, u,
           row == AV_GENDER ? FE_LOOK_GENDER : row == AV_BUILD ? FE_LOOK_BUILD : row == AV_FACE ? FE_LOOK_FACE :
           row == AV_HAIR ? FE_LOOK_HAIR : row == AV_VOICE ? FE_LOOK_VOICE : row == AV_MONTH ? FE_LOOK_MONTH : FE_LOOK_DAY));
}

static void av_type(const fe_unit *u, int row)
{
    fe_save *s = app.save;
    char buf[64];
    if (row == AV_NAME) {
        char cur[48];
        fe_unit_name(s, u, cur, sizeof(cur));
        if (ask_text("Name (max 12)", cur, FE_NAME_MAX, buf, sizeof(buf))) {
            fe_unit_set_name(s, u, buf);
            app.dirty = true;
            fe_unit_name(s, u, cur, sizeof(cur));
            LOGI("avatar edit: renamed to '%s'", cur);
            toast("Renamed to %s (file-select name too)", cur);
        }
    } else if (row == AV_COLOR) {
        char cur[8];
        snprintf(cur, sizeof(cur), "%06lX", (unsigned long)fe_look_color(s, u));
        if (ask_text("Hair color as hex RRGGBB", cur, 6, buf, sizeof(buf))) {
            char *end;
            unsigned long v = strtoul(buf, &end, 16);
            if (*end == '\0' && strlen(buf) == 6) {
                fe_look_set_color(s, u, (uint32_t)v);
                app.dirty = true;
                av_log(u, "hair color", (int)v);
            } else {
                toast("Use 6 hex digits, e.g. 325AB4");
            }
        }
    } else if (row == AV_MONTH || row == AV_DAY) {
        int field = row == AV_MONTH ? FE_LOOK_MONTH : FE_LOOK_DAY, want;
        if (ask_number(AV_LABELS[row], fe_look_get(s, u, field), 2, &want)) {
            int mx = row == AV_MONTH ? 12 : 31;
            fe_look_set(s, u, field, want < 1 ? 1 : want > mx ? mx : want);
            app.dirty = true;
            av_log(u, AV_LABELS[row], fe_look_get(s, u, field));
        }
    } else {
        av_step(u, row, +1);
    }
}

// Moves to the previous/next unit that has Avatar/logbook data
static void av_cycle(int d)
{
    int n = app.save->unit_count;
    for (int k = 1; k <= n; k++) {
        int i = ((app.av_unit + d * k) % n + n) % n;
        if (is_logbook_player(i)) { app.av_unit = i; av_identity_off(&app.save->units[i]); return; }
    }
}

static void screen_avatar(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    if (repeat & KEY_L) av_cycle(-1);
    if (repeat & KEY_R) av_cycle(+1);
    if ((down & KEY_TOUCH) && ui_button_hit(&BTN_AV_PREV, touch)) av_cycle(-1);
    if ((down & KEY_TOUCH) && ui_button_hit(&BTN_AV_NEXT, touch)) av_cycle(+1);
    const fe_unit *u = &s->units[app.av_unit];
    if (repeat & KEY_UP) app.av_row = (app.av_row + AV_ROWS - 1) % AV_ROWS;
    if (repeat & KEY_DOWN) app.av_row = (app.av_row + 1) % AV_ROWS;
    int step = 0;
    if (repeat & KEY_LEFT) step = -1;
    if (repeat & KEY_RIGHT) step = +1;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < AV_ROWS; r++) {
            float y = 1 + r * 20.0f;
            ui_button label = {0, y, 196, 19, NULL}, minus = {200, y, 56, 19, NULL}, plus = {260, y, 56, 19, NULL};
            if (ui_button_hit(&label, touch)) { app.av_row = r; down |= KEY_A; }
            if (ui_button_hit(&minus, touch)) { app.av_row = r; step = -1; }
            if (ui_button_hit(&plus, touch)) { app.av_row = r; step = +1; }
        }
        if (ui_button_hit(&BTN_AV_DONE, touch)) down |= KEY_B;
        if (ui_button_hit(&BTN_AV_PRESET, touch)) down |= KEY_X;
    }
    if (down & KEY_X) {
        app.lp_sel = app.lp_scroll = 0;
        app.lp_count = 0;
        app.st = ST_LOOK_PICK;
        return;
    }
    if (step && app.av_row != AV_NAME) av_step(u, app.av_row, step);
    if (down & (KEY_A | KEY_Y)) av_type(u, app.av_row);
    if (down & KEY_B) { app.st = ST_EXTRAS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    char name[48], face[32];
    fe_unit_name(s, u, name, sizeof(name));
    face_label(fe_look_get(s, u, FE_LOOK_FACE), face, sizeof(face));
    uint32_t col = fe_look_color(s, u);
    char vals[AV_ROWS][48];
    snprintf(vals[AV_NAME], 48, "%s", name);
    snprintf(vals[AV_GENDER], 32, "%s", fe_look_get(s, u, FE_LOOK_GENDER) ? "Female" : "Male");
    snprintf(vals[AV_BUILD], 32, "Build %d", fe_look_get(s, u, FE_LOOK_BUILD) + 1);
    snprintf(vals[AV_FACE], 32, "%s", face);

    snprintf(vals[AV_HAIR], 32, "Hair %d", fe_look_get(s, u, FE_LOOK_HAIR) + 1);
    snprintf(vals[AV_COLOR], 32, "#%06lX", (unsigned long)col);
    snprintf(vals[AV_VOICE], 32, "Voice %d", fe_look_get(s, u, FE_LOOK_VOICE) + 1);
    snprintf(vals[AV_MONTH], 32, "%d", fe_look_get(s, u, FE_LOOK_MONTH));
    snprintf(vals[AV_DAY], 32, "%d", fe_look_get(s, u, FE_LOOK_DAY));

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s - Look & name", name);
    ui_text_right(TOP_W - 8, 3, 0.48f, CLR_DIM, "%s", fe_class_name(fe_unit_class_id(s, u)));
    for (int r = 0; r < AV_ROWS; r++) {
        float y = 26 + r * 16.5f;
        if (r == app.av_row) ui_rect(6, y - 1, TOP_W - 12, 16.5f, CLR_SEL);
        ui_text(14, y, 0.46f, CLR_DIM, "%s", AV_LABELS[r]);
        ui_text(130, y, 0.46f, CLR_TEXT, "%s", vals[r]);
        if (r == AV_COLOR) ui_rect(220, y + 2, 40, 12, C2D_Color32((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, 0xFF));
    }
    const char *note = "";
    if (app.av_row == AV_FACE && face_index(fe_look_get(s, u, FE_LOOK_FACE)) > 4)
        note = "Hero faces change the portrait only; the 3D head stays the Avatar's. Needs the DLC.";
    else if (app.av_row == AV_GENDER)
        note = "Changes body/voice set only. Supports and marriage still follow Avatar (M)/(F).";
    else if (app.av_row == AV_COLOR)
        note = "Left/Right: color presets   A or tap the row: type any hex color";
    if (app.toast_frames > 0) ui_text(12, 194, 0.45f, CLR_GOOD, "%s", app.toast);
    else ui_text(12, 194, 0.42f, CLR_WARN, "%s", note);
    ui_text(8, 222, 0.42f, CLR_DIM, "Left/Right: change  A: type  X: hero look presets  L/R: unit  B: back");

    ui_bottom();
    for (int r = 0; r < AV_ROWS; r++) {
        float y = 1 + r * 20.0f;
        if (r == app.av_row) ui_rect(0, y - 1, BOTTOM_W, 20, CLR_SEL);
        ui_text(6, y + 1, 0.44f, CLR_TEXT, "%s", AV_LABELS[r]);
        ui_text_right(196, y + 1, 0.42f, CLR_ACCENT, "%s", r == AV_COLOR ? "#hex (tap)" : vals[r]);
        if (r != AV_NAME) {
            ui_button minus = {200, y, 56, 18, "-"}, plus = {260, y, 56, 18, "+"};
            ui_button_draw(&minus, true);
            ui_button_draw(&plus, true);
        }
    }
    ui_button_draw(&BTN_AV_PREV, true);
    ui_button_draw(&BTN_AV_NEXT, true);
    ui_button_draw(&BTN_AV_PRESET, true);
    ui_button_draw(&BTN_AV_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Hero look presets (SpotPass/DLC heroes)
//---------------------------------------------------------------------------
#define LP_ROWS 9
static const ui_button BTN_LP_PREV   = {4, 202, 54, 34, "<<"};
static const ui_button BTN_LP_NEXT   = {62, 202, 54, 34, ">>"};
static const ui_button BTN_LP_FILTER = {120, 202, 110, 34, "Filter"};
static const ui_button BTN_LP_CANCEL = {234, 202, 82, 34, "Cancel"};

static void build_look_list(void)
{
    app.lp_count = 0;
    // hero heads first, then the rest
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < FE_LOOK_PRESET_COUNT; i++) {
            bool head = fe_look_presets[i].hero_head;
            if ((pass == 0) != head) continue;
            if (app.lp_heads_only && !head) continue;
            app.lp_ids[app.lp_count++] = i;
        }
}

static void screen_look_pick(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    const fe_unit *u = &s->units[app.av_unit];
    if (app.lp_count == 0) build_look_list();
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_LP_PREV, touch)) repeat |= KEY_L;
        if (ui_button_hit(&BTN_LP_NEXT, touch)) repeat |= KEY_R;
        if (ui_button_hit(&BTN_LP_FILTER, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_LP_CANCEL, touch)) down |= KEY_B;
        for (int r = 0; r < LP_ROWS && app.lp_scroll + r < app.lp_count; r++) {
            ui_button row = {0, 2 + r * 22.0f, BOTTOM_W, 21, NULL};
            if (ui_button_hit(&row, touch)) {
                if (app.lp_sel == app.lp_scroll + r) down |= KEY_A;
                app.lp_sel = app.lp_scroll + r;
            }
        }
    }
    if (down & KEY_X) {
        app.lp_heads_only = !app.lp_heads_only;
        build_look_list();
        app.lp_sel = app.lp_scroll = 0;
    }
    int n = app.lp_count;
    if (n > 0) {
        if (repeat & KEY_UP) app.lp_sel = (app.lp_sel + n - 1) % n;
        if (repeat & KEY_DOWN) app.lp_sel = (app.lp_sel + 1) % n;
        if (repeat & (KEY_L | KEY_LEFT)) app.lp_sel = app.lp_sel >= LP_ROWS ? app.lp_sel - LP_ROWS : 0;
        if (repeat & (KEY_R | KEY_RIGHT)) app.lp_sel = app.lp_sel + LP_ROWS < n ? app.lp_sel + LP_ROWS : n - 1;
        if (app.lp_sel < app.lp_scroll) app.lp_scroll = app.lp_sel;
        if (app.lp_sel >= app.lp_scroll + LP_ROWS) app.lp_scroll = app.lp_sel - LP_ROWS + 1;
    }
    const fe_look_preset *p = n > 0 ? &fe_look_presets[app.lp_ids[app.lp_sel]] : NULL;

    if ((down & KEY_A) && p) {
        fe_look_apply_preset(s, u, p);
        app.dirty = true;
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        LOGI("avatar edit %s: look preset %s%s", name, p->name, p->hero_head ? " (hero head)" : "");
        toast(p->hero_head ? "%s look set (portrait + hair color)" : "Now looks like %s. Name is unchanged.", p->name);
        app.st = ST_AVATAR;
        return;
    }
    if (down & KEY_B) { app.st = ST_AVATAR; return; }

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Hero look presets");
    ui_text_right(TOP_W - 8, 3, 0.48f, CLR_TEXT, "%s (%d)", app.lp_heads_only ? "Hero heads only" : "All heroes", n);
    if (p) {
        char face[32];
        face_label(p->face, face, sizeof(face));
        ui_text(12, 32, 0.65f, CLR_ACCENT, "%s", p->name);
        ui_text(12, 58, 0.48f, p->hero_head ? CLR_GOOD : CLR_DIM, "%s",
                p->hero_head ? "Unique hero head (from DLC)" : "Creator face + signature hair color (no DLC needed)");
        ui_text(12, 82, 0.48f, CLR_TEXT, "%s   Build %d   %s   Hair %d   Voice %d", p->gender ? "Female" : "Male",
                p->build + 1, face, p->hair + 1, p->voice + 1);
        ui_text(12, 104, 0.48f, CLR_TEXT, "Hair color #%06lX", (unsigned long)p->color);
        ui_rect(150, 105, 60, 14, C2D_Color32((p->color >> 16) & 0xFF, (p->color >> 8) & 0xFF, p->color & 0xFF, 0xFF));
        ui_text_wrap(12, 132, 0.42f, CLR_DIM, TOP_W - 24,
                     "Applies gender, build, face, hairstyle, hair color and voice. The name, class and stats are "
                     "not touched. Every field can still be tweaked afterwards.");
    }
    ui_text(8, 222, 0.42f, CLR_DIM, "A: apply look   X: hero heads only / all   L/R: page   B: cancel");

    ui_bottom();
    for (int r = 0; r < LP_ROWS && app.lp_scroll + r < n; r++) {
        int i = app.lp_scroll + r;
        const fe_look_preset *q = &fe_look_presets[app.lp_ids[i]];
        float y = 2 + r * 22.0f;
        if (i == app.lp_sel) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        ui_rect(6, y + 4, 12, 12, C2D_Color32((q->color >> 16) & 0xFF, (q->color >> 8) & 0xFF, q->color & 0xFF, 0xFF));
        ui_text(24, y + 2, 0.48f, CLR_TEXT, "%s", q->name);
        if (q->hero_head) ui_text_right(BOTTOM_W - 6, y + 3, 0.42f, CLR_GOOD, "hero head");
    }
    ui_button_draw(&BTN_LP_PREV, app.lp_scroll > 0);
    ui_button_draw(&BTN_LP_NEXT, app.lp_scroll + LP_ROWS < n);
    ui_button f = BTN_LP_FILTER;
    f.label = app.lp_heads_only ? "Show all" : "Heads only";
    ui_button_draw(&f, true);
    ui_button_draw(&BTN_LP_CANCEL, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Support Log & Gallery (Global file)
//---------------------------------------------------------------------------
#define GL_ROWS 3
#define GLOBAL_UNDO_PATH APP_DIR "/global_undo.bin"
#define BACKUPS_DIR APP_DIR "/backups"
static const ui_button BTN_GL_ROW[GL_ROWS] = {{10, 8, 300, 50, NULL}, {10, 66, 300, 50, NULL}, {10, 124, 300, 50, NULL}};
static const ui_button BTN_GL_BACK = {234, 202, 82, 34, "Back"};

// Complete ON: snapshot the current Global to SD first, so OFF can put it back exactly.
static void global_complete_on(void)
{
    fe_global *g = &app.global;
    FILE *f = fopen(GLOBAL_UNDO_PATH, "wb");
    bool ok = f && fwrite(g->data, 1, g->size, f) == g->size;
    if (f) ok = (fclose(f) == 0) && ok;
    if (!ok) { toast("Could not save the undo copy to SD; not changed."); return; }
    LOGI("global: undo snapshot saved to %s (support %d, gallery %d)", GLOBAL_UNDO_PATH,
         fe_global_support_count(g), fe_global_gallery_count(g));
    fe_global_complete_logs(g);
    app.global_dirty = app.dirty = true;
    LOGI("global edit: support log + gallery completed");
    toast("Completed. Turning it off restores the previous log.");
}

// Complete OFF: restore the log/gallery from the snapshot (Game Clear flag stays as set).
static void global_complete_off(void)
{
    fe_global *g = &app.global;
    FILE *f = fopen(GLOBAL_UNDO_PATH, "rb");
    if (!f) { toast("No undo copy on SD. Use Restore from a backup."); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    fe_global snap;
    char err[96];
    if (ok) ok = fe_global_load_raw(&snap, buf, (size_t)n, err, sizeof(err));
    free(buf);
    if (!ok) { toast("Undo copy unreadable. Use Restore from a backup."); return; }
    bool clear = fe_global_game_clear(g);
    ok = fe_global_copy_logs(g, &snap, err, sizeof(err));
    fe_global_set_game_clear(g, clear);
    fe_global_free(&snap);
    if (!ok) { toast("%s", err); return; }
    app.global_dirty = app.dirty = true;
    LOGI("global edit: support log + gallery restored from undo copy (support %d, gallery %d)",
         fe_global_support_count(g), fe_global_gallery_count(g));
    toast("Previous Support Log and Gallery restored");
}

static int gbk_cmp(const void *a, const void *b)
{
    return -strcmp((const char *)a, (const char *)b);  // newest first (names are dates)
}

static void global_list_backups(void)
{
    app.gbk_count = 0;
    app.gbk_sel = app.gbk_scroll = 0;
    DIR *d = opendir(BACKUPS_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && app.gbk_count < 40) {
        if (e->d_name[0] == '.') continue;
        snprintf(app.gbk[app.gbk_count].name, sizeof(app.gbk[0].name), "%.31s", e->d_name);
        app.gbk_count++;
    }
    closedir(d);
    qsort(app.gbk, app.gbk_count, sizeof(app.gbk[0]), gbk_cmp);
    for (int i = 0; i < app.gbk_count; i++) {
        char path[128];
        snprintf(path, sizeof(path), BACKUPS_DIR "/%s/Global", app.gbk[i].name);
        app.gbk[i].ok = false;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *buf = n > 0 ? malloc((size_t)n) : NULL;
        bool rd = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        fe_global g;
        char err[64];
        if (rd && fe_global_load(&g, buf, (size_t)n, err, sizeof(err))) {
            app.gbk[i].ok = true;
            app.gbk[i].support = fe_global_support_count(&g);
            app.gbk[i].gallery = fe_global_gallery_count(&g);
            app.gbk[i].clear = fe_global_game_clear(&g);
            fe_global_free(&g);
        }
        free(buf);
    }
}

static void screen_global(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_global *g = &app.global;
    if (repeat & KEY_UP) app.global_row = (app.global_row + GL_ROWS - 1) % GL_ROWS;
    if (repeat & KEY_DOWN) app.global_row = (app.global_row + 1) % GL_ROWS;
    int act = -1;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < GL_ROWS; r++)
            if (ui_button_hit(&BTN_GL_ROW[r], touch)) { app.global_row = r; act = r; }
        if (ui_button_hit(&BTN_GL_BACK, touch)) down |= KEY_B;
    }
    if (down & KEY_A) act = app.global_row;
    if (act == 0) {
        bool on = !fe_global_game_clear(g);
        fe_global_set_game_clear(g, on);
        app.global_dirty = app.dirty = true;
        LOGI("global edit: game clear flag %s", on ? "ON" : "OFF");
        toast("Game Clear flag %s", on ? "ON" : "OFF");
    }
    if (act == 1) {
        if (fe_global_is_complete(g)) global_complete_off();
        else global_complete_on();
    }
    if (act == 2) {
        ui_busy("Backups", "Reading backups...");
        global_list_backups();
        app.st = ST_GLOBAL_RESTORE;
        return;
    }
    if (down & KEY_B) { app.st = ST_EXTRAS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    bool clear = fe_global_game_clear(g), complete = fe_global_is_complete(g);
    int sup = fe_global_support_count(g), gal = fe_global_gallery_count(g);

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Support Log & Gallery%s", app.global_dirty ? "  * unsaved" : "");
    ui_text(12, 34, 0.55f, CLR_TEXT, "Game Clear flag:");
    ui_text(200, 34, 0.55f, clear ? CLR_GOOD : CLR_DIM, "%s", clear ? "ON" : "OFF");
    ui_text(12, 58, 0.55f, CLR_TEXT, "Support Log:");
    ui_text(200, 58, 0.55f, sup == FE_SUPPORT_LOG_ENTRIES ? CLR_GOOD : CLR_ACCENT, "%d / %d", sup, FE_SUPPORT_LOG_ENTRIES);
    ui_text(12, 82, 0.55f, CLR_TEXT, "Unit Gallery:");
    ui_text(200, 82, 0.55f, gal == FE_GALLERY_ENTRIES ? CLR_GOOD : CLR_ACCENT, "%d / %d", gal, FE_GALLERY_ENTRIES);
    ui_text_wrap(12, 112, 0.44f, CLR_DIM, TOP_W - 24,
                 "The game shows the Support Log, Theater and Unit Gallery only after a game clear; the flag "
                 "turns them on. Turning Complete on first saves an undo copy to SD, so turning it off puts "
                 "the old log back. All of this is in the shared Global file (every slot). START in the unit "
                 "list saves the slot and Global together.");
    if (app.toast_frames > 0) ui_text(12, 196, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "Up/Down: select   A: apply   B: back");

    ui_bottom();
    const char *titles[GL_ROWS] = {"Game Clear flag", "Complete Support Log & Gallery", "Restore from a backup..."};
    char subs[GL_ROWS][64];
    snprintf(subs[0], 64, "%s  (tap to turn %s)", clear ? "ON" : "OFF", clear ? "off" : "on");
    snprintf(subs[1], 64, "%s  (%d/%d, %d/%d)", complete ? "ON - tap to undo" : "OFF - tap to complete", sup,
             FE_SUPPORT_LOG_ENTRIES, gal, FE_GALLERY_ENTRIES);
    snprintf(subs[2], 64, "copy the log from an earlier backup");
    for (int r = 0; r < GL_ROWS; r++) {
        const ui_button *b = &BTN_GL_ROW[r];
        ui_rect(b->x, b->y, b->w, b->h, app.global_row == r ? CLR_SEL : CLR_BTN);
        ui_text(b->x + 12, b->y + 8, 0.52f, CLR_TEXT, "%s", titles[r]);
        ui_text(b->x + 12, b->y + 28, 0.44f, r == 1 && complete ? CLR_GOOD : CLR_DIM, "%s", subs[r]);
    }
    ui_button_draw(&BTN_GL_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Restore the Support Log / Gallery / Game Clear flag from an automatic backup
//---------------------------------------------------------------------------
#define GBK_ROWS 9
static const ui_button BTN_GB_CANCEL = {234, 202, 82, 34, "Cancel"};

static void screen_global_restore(u32 down, u32 repeat, const touchPosition *touch)
{
    int n = app.gbk_count;
    if (n > 0) {
        if (repeat & KEY_UP) app.gbk_sel = (app.gbk_sel + n - 1) % n;
        if (repeat & KEY_DOWN) app.gbk_sel = (app.gbk_sel + 1) % n;
        if (repeat & (KEY_L | KEY_LEFT)) app.gbk_sel = app.gbk_sel >= GBK_ROWS ? app.gbk_sel - GBK_ROWS : 0;
        if (repeat & (KEY_R | KEY_RIGHT)) app.gbk_sel = app.gbk_sel + GBK_ROWS < n ? app.gbk_sel + GBK_ROWS : n - 1;
    }
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_GB_CANCEL, touch)) down |= KEY_B;
        for (int r = 0; r < GBK_ROWS && app.gbk_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 22.0f, BOTTOM_W, 21, NULL};
            if (ui_button_hit(&row, touch)) {
                if (app.gbk_sel == app.gbk_scroll + r) down |= KEY_A;
                app.gbk_sel = app.gbk_scroll + r;
            }
        }
    }
    if (app.gbk_sel < app.gbk_scroll) app.gbk_scroll = app.gbk_sel;
    if (app.gbk_sel >= app.gbk_scroll + GBK_ROWS) app.gbk_scroll = app.gbk_sel - GBK_ROWS + 1;

    if ((down & KEY_A) && n > 0 && app.gbk[app.gbk_sel].ok) {
        char path[128], err[96];
        snprintf(path, sizeof(path), BACKUPS_DIR "/%s/Global", app.gbk[app.gbk_sel].name);
        u8 *buf = NULL;
        FILE *f = fopen(path, "rb");
        long len = 0;
        if (f) { fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET); buf = malloc((size_t)len); }
        bool ok = f && buf && fread(buf, 1, (size_t)len, f) == (size_t)len;
        if (f) fclose(f);
        fe_global b;
        if (ok) ok = fe_global_load(&b, buf, (size_t)len, err, sizeof(err));
        free(buf);
        if (ok) {
            ok = fe_global_copy_logs(&app.global, &b, err, sizeof(err));
            fe_global_free(&b);
        }
        if (ok) {
            app.global_dirty = app.dirty = true;
            LOGI("global edit: support log/gallery/game clear restored from backup %s", app.gbk[app.gbk_sel].name);
            toast("Restored from backup %s", app.gbk[app.gbk_sel].name);
            app.st = ST_GLOBAL;
        } else {
            toast("Could not use that backup");
        }
        return;
    }
    if (down & KEY_B) { app.st = ST_GLOBAL; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Restore Support Log from a backup");
    if (n == 0) {
        ui_text_wrap(12, 40, 0.5f, CLR_DIM, TOP_W - 24, "No backups found in sdmc:/3ds/FEAEditor/backups/ yet.");
    } else {
        ui_text(12, 36, 0.55f, CLR_ACCENT, "%s", app.gbk[app.gbk_sel].name);
        if (app.gbk[app.gbk_sel].ok) {
            ui_text(12, 62, 0.5f, CLR_TEXT, "Support Log %d / %d", app.gbk[app.gbk_sel].support, FE_SUPPORT_LOG_ENTRIES);
            ui_text(12, 82, 0.5f, CLR_TEXT, "Unit Gallery %d / %d", app.gbk[app.gbk_sel].gallery, FE_GALLERY_ENTRIES);
            ui_text(12, 102, 0.5f, CLR_TEXT, "Game Clear flag %s", app.gbk[app.gbk_sel].clear ? "ON" : "OFF");
        } else {
            ui_text(12, 62, 0.5f, CLR_WARN, "This backup has no readable Global file.");
        }
        ui_text_wrap(12, 132, 0.44f, CLR_DIM, TOP_W - 24,
                     "Copies only the Support Log, Unit Gallery and Game Clear flag from this backup into Global. "
                     "Backups are made automatically every time the editor saves (newest first).");
    }
    ui_text(8, 222, 0.42f, CLR_DIM, "A: restore this one   L/R: page   B: cancel");

    ui_bottom();
    for (int r = 0; r < GBK_ROWS && app.gbk_scroll + r < n; r++) {
        int i = app.gbk_scroll + r;
        float y = 2 + r * 22.0f;
        if (i == app.gbk_sel) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        ui_text(6, y + 2, 0.46f, app.gbk[i].ok ? CLR_TEXT : CLR_DIM, "%s", app.gbk[i].name);
        if (app.gbk[i].ok)
            ui_text_right(BOTTOM_W - 6, y + 3, 0.42f, CLR_DIM, "log %d  gal %d", app.gbk[i].support, app.gbk[i].gallery);
    }
    ui_button_draw(&BTN_GB_CANCEL, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Add a unit
//---------------------------------------------------------------------------
#define ADD_ROWS 9
static const ui_button BTN_AD_PREV = {4, 202, 54, 34, "<<"};
static const ui_button BTN_AD_NEXT = {62, 202, 54, 34, ">>"};
static const ui_button BTN_AD_ADD  = {120, 202, 110, 34, "Add"};
static const ui_button BTN_AD_BACK = {234, 202, 82, 34, "Back"};

static int add_template(void)
{
    int t = fe_find_unit(app.save, 0);
    if (t < 0) t = fe_find_unit(app.save, 1);
    if (t < 0)
        for (int i = 0; i < app.save->unit_count && t < 0; i++)
            if (app.save->units[i].group == FE_GROUP_ARMY) t = i;
    return t;
}

static const char *parent_name(int id)
{
    return id < 0 ? "(none)" : fe_char(id) ? fe_char(id)->name : "?";
}

// Index of a player unit with Avatar logbook data to copy from (-1 if none)
static int logbook_source(int except)
{
    for (int i = 0; i < app.save->unit_count; i++) {
        const fe_unit *u = &app.save->units[i];
        uint16_t c = fe_unit_char_id(app.save, u);
        if (i != except && u->log_off >= 0 && (c == 0 || c == 1) &&
            (u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE || u->group == FE_GROUP_DEAD))
            return i;
    }
    return -1;
}

static int addable_ids[FE_MAX_CHARS + 2];  // [0] = -1: "Import an Avatar..."
static int addable_count = 0;

static void add_list_draw(int n, int id)
{
    fe_save *s = app.save;
    for (int r = 0; r < ADD_ROWS && app.add_scroll + r < n; r++) {
        int c = addable_ids[app.add_scroll + r];
        float yy = 2 + r * 22.0f;
        if (c < 0) {
            if (app.add_scroll + r == app.add_sel) ui_rect(0, yy - 1, BOTTOM_W, 22, CLR_SEL);
            ui_text(8, yy + 2, 0.48f, CLR_ACCENT, "Import an Avatar...");
            continue;
        }
        if (app.add_scroll + r == app.add_sel) ui_rect(0, yy - 1, BOTTOM_W, 22, c == app.add_armed ? CLR_ACCENT : CLR_SEL);
        bool have = fe_find_unit(s, c) >= 0;
        const fe_char_recruit *crc = fe_char_recruit_of(c);
        int vid = fe_char_vanilla_id(c);
        bool kc = crc != NULL, child = kc && crc->parent >= 0;
        ui_text(8, yy + 2, 0.48f, have ? CLR_DIM : kc ? CLR_TEXT : CLR_WARN, "%s", char_name(c));
        ui_text_right(BOTTOM_W - 6, yy + 3, 0.42f, have ? CLR_DIM : child ? CLR_GOOD : vid < 0 ? CLR_WARN : CLR_DIM,
                      "%s", have ? "in army" : child ? "child" : vid < 0 ? "mod" : vid >= 53 ? "test" : "");
    }
    ui_button_draw(&BTN_AD_PREV, app.add_scroll > 0);
    ui_button_draw(&BTN_AD_NEXT, app.add_scroll + ADD_ROWS < n);
    ui_button b = BTN_AD_ADD;
    if (id < 0) b.label = "Open";
    if (id >= 0 && app.add_armed == id) b.label = "Confirm";
    ui_button_draw(&b, true);
    ui_button_draw(&BTN_AD_BACK, true);
}

static void screen_add_unit(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    if (!addable_count) {
        addable_ids[addable_count++] = -1;
        for (int c = 0; c < fe_char_count() && addable_count < FE_MAX_CHARS + 2; c++)
            if (fe_char_addable(c)) addable_ids[addable_count++] = c;
    }
    int n = addable_count;
    int before = app.add_sel;
    if (repeat & KEY_UP) app.add_sel = (app.add_sel + n - 1) % n;
    if (repeat & KEY_DOWN) app.add_sel = (app.add_sel + 1) % n;
    if (repeat & (KEY_L | KEY_LEFT)) app.add_sel = app.add_sel >= ADD_ROWS ? app.add_sel - ADD_ROWS : 0;
    if (repeat & (KEY_R | KEY_RIGHT)) app.add_sel = app.add_sel + ADD_ROWS < n ? app.add_sel + ADD_ROWS : n - 1;
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_AD_PREV, touch)) app.add_sel = app.add_scroll >= ADD_ROWS ? app.add_scroll - ADD_ROWS : 0;
        if (ui_button_hit(&BTN_AD_NEXT, touch)) app.add_sel = app.add_scroll + ADD_ROWS < n ? app.add_scroll + ADD_ROWS : n - 1;
        if (ui_button_hit(&BTN_AD_ADD, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_AD_BACK, touch)) down |= KEY_B;
        for (int r = 0; r < ADD_ROWS && app.add_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 22.0f, BOTTOM_W, 21, NULL};
            if (ui_button_hit(&row, touch)) app.add_sel = app.add_scroll + r;
        }
    }
    if (app.add_sel != before) app.add_armed = -1;
    if (app.add_sel < app.add_scroll) app.add_scroll = app.add_sel;
    if (app.add_sel >= app.add_scroll + ADD_ROWS) app.add_scroll = app.add_sel - ADD_ROWS + 1;

    int id = addable_ids[app.add_sel];
    if (id < 0) {
        if (down & KEY_A) { app.add_armed = -1; app.imp_menu_sel = 0; app.toast_frames = 0; app.st = ST_IMPORT_MENU; return; }
        if (down & KEY_B) { app.add_armed = -1; app.st = ST_EXTRAS; return; }
        ui_begin();
        ui_top();
        ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
        ui_text(8, 3, 0.55f, CLR_ACCENT, "Add a unit%s", app.dirty ? "  * unsaved" : "");
        ui_text(12, 32, 0.65f, CLR_ACCENT, "Import an Avatar");
        ui_text_wrap(12, 60, 0.48f, CLR_TEXT, TOP_W - 24,
                     "Bring in an Avatar from another save slot, a StreetPass team, your logbook or a "
                     "Checkpoint backup, exactly as it was (look, class, stats, skills, items). It joins "
                     "as an (M)/(F) Robin unit, so it can build supports.");
        ui_text(8, 222, 0.42f, CLR_DIM, "A: open   L/R: page   B: back");
        ui_bottom();
        add_list_draw(n, id);
        ui_end();
        return;
    }
    static const fe_char_recruit unknown_rc = {.parent = -1};
    bool known = fe_char_recruit_of(id) != NULL;
    const fe_char_recruit *rc = known ? fe_char_recruit_of(id) : &unknown_rc;
    bool child = rc->parent >= 0;
    const char *nm = char_name(id);
    char nmbuf[24];
    snprintf(nmbuf, sizeof(nmbuf), "%s", nm);
    nm = nmbuf;
    int father = -1, mother = -1;
    if (child) fe_unit_add_parents(s, id, &father, &mother);

    if (down & KEY_A) {
        if (app.add_armed != id) {
            app.add_armed = id;
            toast("Press A again to add %s", nm);
        } else {
            char err[160];
            int t = add_template();
            ui_busy("Adding unit", "Inserting the new unit into the save...");
            int ni = t >= 0 ? fe_unit_add(s, t, id, err, sizeof(err)) : -1;
            app.add_armed = -1;
            if (ni < 0) {
                if (t < 0) snprintf(err, sizeof(err), "no unit to use as a template");
                LOGE("add unit %s failed: %s", nm, err);
                toast("Could not add: %s", err);
            } else {
                // Avatars need logbook data, or the game shows "Unknown" with a silhouette
                if (id == 0 || id == 1) {
                    int src = logbook_source(ni);
                    if (src < 0 || !fe_unit_add_logbook(s, ni, src, err, sizeof(err)))
                        LOGW("added %s without logbook data: %s", nm, src < 0 ? "no source" : err);
                    else {
                        LOGI("gave %s logbook data (name Robin) copied from unit %d", nm, src);
                    }
                }
                app.dirty = true;
                build_visible_list();
                for (int v = 0; v < app.vis_count; v++)
                    if (app.visible[v] == ni) app.unit_sel = v;
                LOGI("added unit %s (%s, Lv1)%s%s%s%s", nm, fe_class_name(fe_unit_class_id(s, &s->units[ni])),
                     child ? " father " : "", child ? parent_name(father) : "", child ? " mother " : "",
                     child ? parent_name(mother) : "");
                toast("Added %s at the end of the army list", nm);
            }
        }
    }
    if (down & KEY_B) { app.add_armed = -1; app.st = ST_EXTRAS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Add a unit%s", app.dirty ? "  * unsaved" : "");
    ui_text(12, 32, 0.65f, CLR_ACCENT, "%s", nm);
    if (known)
        ui_text(12, 58, 0.5f, CLR_TEXT, "%s   Lv 1   %s", fe_class_name(rc->start_class), rc->female ? "Female" : "Male");
    else
        ui_text(12, 58, 0.5f, CLR_TEXT, "Unknown here   Lv 1   class: template's");
    int y = 80;
    if (fe_find_unit(s, id) >= 0) { ui_text(12, y, 0.46f, CLR_WARN, "Already in this army: this adds a second copy."); y += 18; }
    if (child) {
        ui_text(12, y, 0.48f, CLR_TEXT, "Father: %s    Mother: %s", parent_name(father), parent_name(mother));
        y += 18;
        if (father < 0 || mother < 0) {
            ui_text_wrap(12, y, 0.44f, CLR_WARN, TOP_W - 24,
                         "One parent is missing: give the fixed parent an S support first (Supports screen) "
                         "if you want both parents filled in.");
            y += 30;
        }
    }
    if (id == 0 || id == 1)
        ui_text(12, y, 0.44f, CLR_DIM, "Joins with Robin's default look.");
    if (id >= 53) {
        ui_text_wrap(12, y, 0.44f, CLR_WARN, TOP_W - 24,
                     "Special character. May not behave like a normal unit.");
        y += 30;
    }
    ui_text_wrap(12, 150, 0.42f, CLR_DIM, TOP_W - 24,
                 "New units start at level 1 with base stats, the character's personal skills, an empty "
                 "inventory and no supports. Edit them like any other unit afterwards. Nothing is written "
                 "until you save (START in the unit list).");
    if (app.toast_frames > 0) ui_text(12, 196, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "A, A: add   L/R: page   B: back");

    ui_bottom();
    add_list_draw(n, id);
    ui_end();
}

//---------------------------------------------------------------------------
// More (per unit): weapon ranks, Boots, battle records, revive, remove
//---------------------------------------------------------------------------
enum { MO_BOOTS = FE_WEAPON_COUNT, MO_BATTLES, MO_VICTORIES, MO_ROWS };
static const ui_button BTN_MO_REVIVE  = {4, 206, 72, 30, "Revive"};
static const ui_button BTN_MO_REMOVE  = {80, 206, 72, 30, "Remove"};
static const ui_button BTN_MO_PARENTS = {156, 206, 82, 30, "Parents"};

static const char *more_label(int r)
{
    return r < FE_WEAPON_COUNT ? fe_weapon_names[r] : r == MO_BOOTS ? "Boots (Move +)" : r == MO_BATTLES ? "Battles" : "Victories";
}

static int more_get(const fe_unit *u, int r)
{
    fe_save *s = app.save;
    if (r < FE_WEAPON_COUNT) return fe_unit_wexp(s, u, r);
    if (r == MO_BOOTS) return fe_unit_boots(s, u);
    return fe_unit_record(s, u, r == MO_VICTORIES);
}

static int more_max(int r)
{
    return r < FE_WEAPON_COUNT ? FE_WEXP_MAX : r == MO_BOOTS ? 9 : 9999;
}

static void more_set(const fe_unit *u, int r, int v)
{
    fe_save *s = app.save;
    int old = more_get(u, r);
    if (r < FE_WEAPON_COUNT) fe_unit_set_wexp(s, u, r, v);
    else if (r == MO_BOOTS) fe_unit_set_boots(s, u, v);
    else fe_unit_set_record(s, u, r == MO_VICTORIES, v);
    int now = more_get(u, r);
    if (now != old) {
        char name[48];
        fe_unit_name(s, u, name, sizeof(name));
        app.dirty = true;
        LOGI("edit %s: %s %d -> %d", name, more_label(r), old, now);
    }
}

static void more_edit(const fe_unit *u, int r, int how)  // how: -1/+1 step, -2/+2 big step, 3 max, 4 type
{
    int v = more_get(u, r);
    if (how == 3) v = more_max(r);
    else if (how == 4) {
        if (!ask_number(more_label(r), v, r < MO_BATTLES ? 2 : 5, &v)) return;
    } else if (how == 2 || how == -2)
        v = r < FE_WEAPON_COUNT ? (how > 0 ? fe_wexp_next_rank(v) : fe_wexp_prev_rank(v)) : v + (how > 0 ? 10 : -10);
    else
        v += how;
    if (v < 0) v = 0;
    more_set(u, r, v);
}

static void select_unit_index(int idx)
{
    build_visible_list();
    for (int v = 0; v < app.vis_count; v++)
        if (app.visible[v] == idx) app.unit_sel = v;
    if (app.unit_sel >= LIST_ROWS) app.unit_scroll = app.unit_sel - LIST_ROWS + 1;
}

static void screen_more(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    const fe_unit *u = selected_unit();
    if (!u) { app.st = ST_UNITS; return; }
    int ui_idx = app.visible[app.unit_sel];
    bool fallen = u->group == FE_GROUP_DEAD;
    int before = app.more_row;

    if (repeat & KEY_UP) app.more_row = (app.more_row + MO_ROWS - 1) % MO_ROWS;
    if (repeat & KEY_DOWN) app.more_row = (app.more_row + 1) % MO_ROWS;
    if (repeat & KEY_LEFT) more_edit(u, app.more_row, -1);
    if (repeat & KEY_RIGHT) more_edit(u, app.more_row, +1);
    if (repeat & KEY_L) more_edit(u, app.more_row, -2);
    if (repeat & KEY_R) more_edit(u, app.more_row, +2);
    if (down & KEY_X) more_edit(u, app.more_row, 3);
    if (down & KEY_Y) more_edit(u, app.more_row, 4);
    int action = 0;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < MO_ROWS; r++) {
            float y = edit_row_y(r);
            ui_button minus = {150, y, 40, 20, NULL}, plus = {196, y, 40, 20, NULL}, max = {242, y, 70, 20, NULL};
            ui_button label = {0, y, 146, 20, NULL};
            if (ui_button_hit(&minus, touch)) { app.more_row = r; more_edit(u, r, -1); }
            if (ui_button_hit(&plus, touch)) { app.more_row = r; more_edit(u, r, +1); }
            if (ui_button_hit(&max, touch)) { app.more_row = r; more_edit(u, r, 3); }
            if (ui_button_hit(&label, touch)) { app.more_row = r; more_edit(u, r, 4); }
        }
        if (fallen && ui_button_hit(&BTN_MO_REVIVE, touch)) action = 1;
        if (ui_button_hit(&BTN_MO_REMOVE, touch)) action = 2;
        if (u->child_off >= 0 && ui_button_hit(&BTN_MO_PARENTS, touch)) down |= KEY_SELECT;
        if (ui_button_hit(&BTN_DONE, touch)) down |= KEY_B;
    }
    if (app.more_row != before) app.more_armed = 0;
    if (app.more_armed == 2) {  // removal popup: A confirms, B cancels
        if (down & KEY_B) { app.more_armed = 0; down &= ~(u32)KEY_B; }
        else if (down & KEY_A) { action = 2; down &= ~(u32)KEY_A; }
    }
    if ((down & KEY_SELECT) && u->child_off >= 0) {
        app.par_row = 0;
        app.toast_frames = 0;
        app.st = ST_PARENTS;
        return;
    }
    if (action) {
        char name[48], err[160];
        fe_unit_name(s, u, name, sizeof(name));
        if (app.more_armed != action) {
            app.more_armed = action;
            if (action == 1) toast("Tap Revive again to bring %s back", name);
            else app.toast_frames = 0;
        } else {
            app.more_armed = 0;
            uint16_t cid = fe_unit_char_id(s, u);
            int n_before = s->unit_count;
            ui_busy(action == 1 ? "Reviving" : "Removing", name);
            bool ok = action == 1 ? fe_unit_revive(s, ui_idx, err, sizeof(err)) : fe_unit_remove(s, ui_idx, err, sizeof(err));
            if (!ok) {
                LOGE("%s %s failed: %s", action == 1 ? "revive" : "remove", name, err);
                toast("Failed: %s", err);
            } else if (action == 1) {
                app.dirty = true;
                LOGI("revived %s (now at the end of the army, full HP)", name);
                int ni = -1;
                for (int i = 0; i < s->unit_count; i++)
                    if (s->units[i].group == FE_GROUP_ARMY && fe_unit_char_id(s, &s->units[i]) == cid) ni = i;
                select_unit_index(ni);
                toast("%s revived", name);
            } else {
                app.dirty = true;
                LOGI("removed %s (%d -> %d units)", name, n_before, s->unit_count);
                int keep = app.unit_sel;
                build_visible_list();
                app.unit_sel = keep < app.vis_count ? keep : app.vis_count - 1;
                if (app.unit_sel < 0) app.unit_sel = 0;
                app.st = ST_UNITS;
                return;
            }
        }
    }
    if (down & (KEY_B | KEY_A)) { app.more_armed = 0; app.st = ST_EDIT; return; }
    if (app.toast_frames > 0) app.toast_frames--;
    u = selected_unit();
    if (!u) { app.st = ST_UNITS; return; }
    fallen = u->group == FE_GROUP_DEAD;

    char name[48];
    fe_unit_name(s, u, name, sizeof(name));
    uint16_t cid = fe_unit_char_id(s, u);
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    if (app.more_armed == 2) {  // removal popup (alone on the top screen: text draws above shapes)
        bool story = cid == 3 || app.visible[app.unit_sel] == first_avatar();
        ui_rect(30, 34, TOP_W - 60, 176, CLR_ERR);
        ui_rect(33, 37, TOP_W - 66, 170, CLR_BG);
        ui_text(46, 46, 0.62f, CLR_WARN, "Remove %s?", name);
        ui_text_wrap(46, 74, 0.48f, CLR_TEXT, TOP_W - 92,
                     "This removes them from your army entirely. Only use this if you are sure you "
                     "want to remove this character from your game.");
        if (story)
            ui_text_wrap(46, 128, 0.46f, CLR_WARN, TOP_W - 92, "This is a story unit: removing it may break the story.");
        ui_text(46, 170, 0.5f, CLR_ACCENT, "Tap Confirm or press A to remove");
        ui_text(46, 188, 0.5f, CLR_TEXT, "Press B to cancel removal");
    } else {
        ui_text(8, 3, 0.55f, CLR_ACCENT, "%s", name);
        ui_text_right(TOP_W - 8, 3, 0.55f, fallen ? CLR_ERR : CLR_DIM, "%s", fallen ? "Fallen" : fe_class_name(fe_unit_class_id(s, u)));
        for (int r = 0; r < MO_ROWS; r++) {
            float y = 28 + r * 19;
            int v = more_get(u, r);
            if (r == app.more_row) ui_rect(4, y - 1, TOP_W - 8, 18, CLR_SEL);
            ui_text(12, y, 0.55f, CLR_TEXT, "%s", more_label(r));
            if (r < FE_WEAPON_COUNT) {
                ui_text(150, y, 0.55f, v >= FE_WEXP_MAX ? CLR_GOOD : CLR_ACCENT, "%s", fe_wexp_rank(v));
                ui_text_right(210, y, 0.55f, CLR_TEXT, "%d", v);
                ui_rect(220, y + 4, 160, 8, CLR_PANEL);
                ui_rect(220, y + 4, 160.0f * v / FE_WEXP_MAX, 8, v >= FE_WEXP_MAX ? CLR_GOOD : CLR_ACCENT);
            } else
                ui_text_right(210, y, 0.55f, CLR_TEXT, "%d", v);
        }
        if (app.toast_frames > 0)
            ui_text(8, 204, 0.46f, CLR_GOOD, "%s", app.toast);
        else
            ui_text(8, 206, 0.42f, CLR_DIM, "Left/Right: -1/+1  L/R: rank (weapons) or 10  X: max  Y: type");
        ui_text(8, 222, 0.42f, CLR_DIM, "Weapon exp D 15, C 35, B 60, A 90. Class must use the weapon.");
    }

    ui_bottom();
    for (int r = 0; r < MO_ROWS; r++) {
        float y = edit_row_y(r);
        if (r == app.more_row) ui_rect(0, y - 1, BOTTOM_W, 22, CLR_SEL);
        int v = more_get(u, r);
        ui_text(8, y + 1, 0.5f, CLR_TEXT, "%s", more_label(r));
        if (r < FE_WEAPON_COUNT)
            ui_text_right(140, y + 1, 0.5f, CLR_ACCENT, "%s %d", fe_wexp_rank(v), v);
        else
            ui_text_right(140, y + 1, 0.5f, CLR_TEXT, "%d", v);
        ui_button minus = {150, y, 40, 20, "-"}, plus = {196, y, 40, 20, "+"}, max = {242, y, 70, 20, "Max"};
        ui_button_draw(&minus, true);
        ui_button_draw(&plus, true);
        ui_button_draw(&max, true);
    }
    ui_button rv = BTN_MO_REVIVE, rm = BTN_MO_REMOVE;
    if (app.more_armed == 1) rv.label = "Confirm";
    if (app.more_armed == 2) rm.label = "Confirm";
    ui_button_draw(&rv, fallen);
    ui_button_draw(&rm, true);
    ui_button_draw(&BTN_MO_PARENTS, u->child_off >= 0);
    ui_button_draw(&BTN_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Army-wide tools
//---------------------------------------------------------------------------
enum { AT_STATS, AT_SKILLS, AT_WEAPONS, AT_SUPPORTS, AT_HEAL, AT_REVIVE, AT_COUNT };
static const char *const AT_NAMES[AT_COUNT] = {"Max every stat", "Learn every skill", "Max weapon ranks (A)",
                                               "Supports: next conversation", "Heal everyone", "Revive all fallen"};
static const char *const AT_INFO[AT_COUNT] = {
    "Every stat of every player unit to its cap (class max + personal modifiers). HP is refilled.",
    "Every player unit learns all skills, so any of them can be equipped in game.",
    "All six weapon types to A for every player unit. Only weapons the class can use show up in game.",
    "Every pair whose partner is in the army moves to its next \"ready\" rank, up to A ready, "
    "so the conversations can be watched. S stays manual (Supports screen).",
    "Current HP to max for every player unit.",
    "Every fallen unit (Classic) returns to the end of the army with full HP."};
static const ui_button BTN_AT_BACK = {234, 202, 82, 34, "Back"};

static bool is_player(const fe_unit *u)
{
    return u->group == FE_GROUP_ARMY || u->group == FE_GROUP_BLUE || u->group == FE_GROUP_DEAD;
}

static int run_army_tool(int tool, char *msg, size_t msglen)
{
    fe_save *s = app.save;
    char err[160];
    int n = 0;
    if (tool == AT_REVIVE) {
        for (;;) {
            int di = -1;
            for (int i = 0; i < s->unit_count && di < 0; i++)
                if (s->units[i].group == FE_GROUP_DEAD) di = i;
            if (di < 0) break;
            char name[48];
            fe_unit_name(s, &s->units[di], name, sizeof(name));
            if (!fe_unit_revive(s, di, err, sizeof(err))) {
                LOGE("revive %s failed: %s", name, err);
                snprintf(msg, msglen, "Stopped: %.80s", err);
                return n;
            }
            LOGI("army tool: revived %s", name);
            n++;
        }
        build_visible_list();
        snprintf(msg, msglen, n ? "%d unit%s revived" : "No fallen units", n, n == 1 ? "" : "s");
        return n;
    }
    int pairs = 0;
    for (int i = 0; i < s->unit_count; i++) {
        const fe_unit *u = &s->units[i];
        if (!is_player(u)) continue;
        bool changed = false;
        fe_stats st;
        switch (tool) {
        case AT_STATS:
            if (fe_unit_stats(s, u, &st))
                for (int k = 0; k < FE_STAT_COUNT; k++)
                    if (st.value[k] < st.cap[k]) { fe_unit_set_stat(s, u, k, st.cap[k]); changed = true; }
            changed |= fe_unit_heal(s, u);
            break;
        case AT_SKILLS:
            changed = fe_unit_learn_all(s, u) > 0;
            break;
        case AT_WEAPONS:
            for (int w = 0; w < FE_WEAPON_COUNT; w++)
                if (fe_unit_wexp(s, u, w) < FE_WEXP_MAX) { fe_unit_set_wexp(s, u, w, FE_WEXP_MAX); changed = true; }
            break;
        case AT_HEAL:
            changed = fe_unit_heal(s, u);
            break;
        case AT_SUPPORTS: {
            int c = fe_unit_char_id(s, u);
            const fe_support_list *cl = fe_char_supports(c);
            if (!cl || fe_find_unit(s, c) != i) break;  // one record per character
            for (int k = 0; k < cl->count; k++) {
                int partner = cl->partners[k].partner, type = cl->partners[k].type;
                if (partner == FE_SUPPORT_EMPTY || type > 3) continue;
                if (partner <= c || fe_find_unit(s, partner) < 0) continue;  // each pair once
                int v = fe_support_get(s, &s->units[i], partner);
                if (v < 0) continue;
                int nr = fe_support_next_ready(type, v);
                char rank[16];
                if (nr < 0) continue;
                fe_support_rank_name(type, nr, rank, sizeof(rank));
                if (rank[0] == 'S') continue;  // S stays a manual choice
                if (!fe_support_set(s, i, partner, nr, err, sizeof(err))) {
                    LOGE("support %s - %s failed: %s", char_name(c), char_name(partner), err);
                    continue;
                }
                pairs++;
                changed = true;
            }
            break;
        }
        }
        if (changed) n++;
    }
    if (tool == AT_SUPPORTS)
        snprintf(msg, msglen, "%d pair%s now have a conversation ready", pairs, pairs == 1 ? "" : "s");
    else
        snprintf(msg, msglen, "%s: %d unit%s changed", AT_NAMES[tool], n, n == 1 ? "" : "s");
    LOGI("army tool: %s", msg);
    return tool == AT_SUPPORTS ? pairs : n;
}

static void screen_army_tools(u32 down, u32 repeat, const touchPosition *touch)
{
    int before = app.tools_sel;
    if (repeat & KEY_UP) app.tools_sel = (app.tools_sel + AT_COUNT - 1) % AT_COUNT;
    if (repeat & KEY_DOWN) app.tools_sel = (app.tools_sel + 1) % AT_COUNT;
    if (down & KEY_TOUCH) {
        for (int i = 0; i < AT_COUNT; i++) {
            ui_button b = {10, 6 + i * 32.0f, 300, 28, NULL};
            if (ui_button_hit(&b, touch)) {
                if (app.tools_sel == i) down |= KEY_A;
                app.tools_sel = i;
            }
        }
        if (ui_button_hit(&BTN_AT_BACK, touch)) down |= KEY_B;
    }
    if (app.tools_sel != before) app.tools_armed = -1;
    if (down & KEY_A) {
        if (app.tools_armed != app.tools_sel) {
            app.tools_armed = app.tools_sel;
            toast("Press A again to apply to every player unit");
        } else {
            app.tools_armed = -1;
            char msg[96];
            ui_busy("Applying", AT_NAMES[app.tools_sel]);
            if (run_army_tool(app.tools_sel, msg, sizeof(msg)) > 0) app.dirty = true;
            toast("%s", msg);
        }
    }
    if (down & KEY_B) { app.tools_armed = -1; app.st = ST_EXTRAS; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Army-wide tools%s", app.dirty ? "  * unsaved" : "");
    ui_text(12, 36, 0.62f, CLR_ACCENT, "%s", AT_NAMES[app.tools_sel]);
    ui_text_wrap(12, 64, 0.48f, CLR_TEXT, TOP_W - 24, AT_INFO[app.tools_sel]);
    ui_text(12, 140, 0.44f, CLR_DIM, "Player units = army, deployed and fallen. Nothing is written until you save.");
    if (app.toast_frames > 0) ui_text(12, 190, 0.5f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "A, A: apply   B: back");
    ui_bottom();
    for (int i = 0; i < AT_COUNT; i++) {
        ui_button b = {10, 6 + i * 32.0f, 300, 28, AT_NAMES[i]};
        if (i == app.tools_armed) b.label = "Confirm: tap again";
        if (i == app.tools_sel) ui_rect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, i == app.tools_armed ? CLR_ACCENT : CLR_SEL);
        ui_button_draw(&b, true);
    }
    ui_button_draw(&BTN_AT_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// World map & chapters
//---------------------------------------------------------------------------
#define MAP_ROWS 8
enum { MS_LOCKED, MS_OPEN, MS_BEATEN, MS_RISEN, MS_MERCHANT };
static const char *const MS_NAMES[5] = {"Locked", "Open", "Beaten", "Risen", "Merchant"};
static const ui_button BTN_MP_ALL   = {4, 202, 112, 34, "Set all..."};
static const ui_button BTN_MP_STORY = {120, 202, 110, 34, "Story here"};
static const ui_button BTN_MP_BACK  = {234, 202, 82, 34, "Back"};
#define MAP_ALL_COUNT 6
static const char *const MAP_ALL_NAMES[MAP_ALL_COUNT] = {
    "Open every location", "Beat every location", "Lock every location",
    "Risen on all unlocked", "Merchants on all unlocked", "Clear all encounters"};
static const char *const MAP_ALL_INFO[MAP_ALL_COUNT] = {
    "Every location becomes Open: playable from the world map, including beaten chapters.",
    "Every location becomes Beaten (cleared).",
    "Every location becomes Locked (hidden), and its encounters are cleared.",
    "Puts a Risen battle on every unlocked location (open or beaten), for grinding.",
    "Puts a traveling merchant on every unlocked location.",
    "Removes every Risen battle and merchant. StreetPass/SpotPass teams stay."};

static uint32_t map_rand(void)
{
    static uint32_t x = 0;
    if (!x) x = (uint32_t)time(NULL) * 2654435761u | 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static int map_show_state(int loc)
{
    int enc = fe_map_encounter(app.save, loc), st = fe_map_state(app.save, loc);
    if (enc == FE_ENC_RISEN) return MS_RISEN;
    if (enc == FE_ENC_MERCHANT) return MS_MERCHANT;
    return st == FE_MAP_BEATEN ? MS_BEATEN : st == FE_MAP_OPEN ? MS_OPEN : MS_LOCKED;
}

static void map_set_show(int loc, int ms)
{
    fe_save *s = app.save;
    int old = map_show_state(loc);
    static const int lock[5] = {FE_MAP_LOCKED, FE_MAP_OPEN, FE_MAP_BEATEN, FE_MAP_BEATEN, FE_MAP_BEATEN};
    static const int enc[5] = {FE_ENC_NONE, FE_ENC_NONE, FE_ENC_NONE, FE_ENC_RISEN, FE_ENC_MERCHANT};
    fe_map_set_state(s, loc, lock[ms]);
    fe_map_set_encounter(s, loc, enc[ms], map_rand());
    app.dirty = true;
    LOGI("world map: %s %s -> %s", fe_map_name(loc), MS_NAMES[old], MS_NAMES[ms]);
}

static void map_cycle(int loc, int dir)
{
    map_set_show(loc, (map_show_state(loc) + 5 + dir) % 5);
}

static void map_set_all(int what)
{
    fe_save *s = app.save;
    int n = 0;
    for (int loc = 0; loc < s->map_count; loc++) {
        int st = fe_map_state(s, loc);
        if (what == 0) { fe_map_set_state(s, loc, FE_MAP_OPEN); n++; }
        else if (what == 1) { fe_map_set_state(s, loc, FE_MAP_BEATEN); n++; }
        else if (what == 2) { fe_map_set_state(s, loc, FE_MAP_LOCKED); fe_map_set_encounter(s, loc, FE_ENC_NONE, 0); n++; }
        else if (what == 3 || what == 4) {
            if (st == FE_MAP_LOCKED) continue;
            fe_map_set_encounter(s, loc, what == 3 ? FE_ENC_RISEN : FE_ENC_MERCHANT, map_rand());
            n++;
        } else { fe_map_set_encounter(s, loc, FE_ENC_NONE, 0); n++; }
    }
    app.dirty = true;
    LOGI("world map: %s (%d locations)", MAP_ALL_NAMES[what], n);
    toast("%s: %d locations", MAP_ALL_NAMES[what], n);
}

static void screen_world_map_all(u32 down, u32 repeat, const touchPosition *touch)
{
    if (repeat & KEY_UP) app.map_popup_sel = (app.map_popup_sel + MAP_ALL_COUNT - 1) % MAP_ALL_COUNT;
    if (repeat & KEY_DOWN) app.map_popup_sel = (app.map_popup_sel + 1) % MAP_ALL_COUNT;
    int pick = -1;
    if (down & KEY_A) pick = app.map_popup_sel;
    if (down & KEY_TOUCH) {
        for (int i = 0; i < MAP_ALL_COUNT; i++) {
            ui_button b = {10, 6 + i * 32.0f, 300, 28, NULL};
            if (ui_button_hit(&b, touch)) pick = i;
        }
        if (ui_button_hit(&BTN_MP_BACK, touch)) down |= KEY_B;
    }
    if (pick >= 0) { map_set_all(pick); app.map_popup = false; return; }
    if (down & KEY_B) { app.map_popup = false; return; }

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "World map - set all");
    ui_text(12, 36, 0.62f, CLR_ACCENT, "%s", MAP_ALL_NAMES[app.map_popup_sel]);
    ui_text_wrap(12, 64, 0.48f, CLR_TEXT, TOP_W - 24, MAP_ALL_INFO[app.map_popup_sel]);
    ui_text_wrap(12, 150, 0.44f, CLR_WARN, TOP_W - 24, "Experimental: test on a copy of your save first.");
    ui_text(8, 222, 0.42f, CLR_DIM, "A or tap: apply   B: cancel");
    ui_bottom();
    for (int i = 0; i < MAP_ALL_COUNT; i++) {
        ui_button b = {10, 6 + i * 32.0f, 300, 28, MAP_ALL_NAMES[i]};
        if (i == app.map_popup_sel) ui_rect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, CLR_SEL);
        ui_button_draw(&b, true);
    }
    ui_button c = BTN_MP_BACK;
    c.label = "Cancel";
    ui_button_draw(&c, true);
    ui_end();
}

static void screen_world_map(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    int n = s->map_count;
    if (app.map_popup) { screen_world_map_all(down, repeat, touch); return; }
    if (repeat & KEY_UP) app.map_sel = (app.map_sel + n - 1) % n;
    if (repeat & KEY_DOWN) app.map_sel = (app.map_sel + 1) % n;
    if (repeat & KEY_L) app.map_sel = app.map_sel >= MAP_ROWS ? app.map_sel - MAP_ROWS : 0;
    if (repeat & KEY_R) app.map_sel = app.map_sel + MAP_ROWS < n ? app.map_sel + MAP_ROWS : n - 1;
    if (repeat & KEY_LEFT) map_cycle(app.map_sel, -1);
    if (repeat & KEY_RIGHT) map_cycle(app.map_sel, +1);
    if (down & KEY_A) map_cycle(app.map_sel, +1);
    if (down & KEY_TOUCH) {
        for (int r = 0; r < MAP_ROWS && app.map_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 24.0f, BOTTOM_W, 23, NULL};
            if (ui_button_hit(&row, touch)) {
                if (app.map_sel == app.map_scroll + r) map_cycle(app.map_sel, +1);
                app.map_sel = app.map_scroll + r;
            }
        }
        if (ui_button_hit(&BTN_MP_ALL, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_MP_STORY, touch)) down |= KEY_SELECT;
        if (ui_button_hit(&BTN_MP_BACK, touch)) down |= KEY_B;
    }
    if (down & KEY_X) { app.map_popup = true; app.map_popup_sel = 0; return; }
    bool story_loc = app.map_sel <= 26;  // Prologue .. Endgame
    if ((down & KEY_SELECT) && story_loc) {
        int old = fe_story_chapter(s), id = app.map_sel + 2;
        if (old != id) {
            char was[24];
            snprintf(was, sizeof(was), "%s", fe_chapter_name(old));
            fe_story_set_chapter(s, id);
            if (fe_map_state(s, app.map_sel) != FE_MAP_OPEN) fe_map_set_state(s, app.map_sel, FE_MAP_OPEN);
            app.dirty = true;
            LOGI("story chapter: %s -> %s (location set Open)", was, fe_chapter_name(id));
            toast("Story chapter set to %s", fe_map_name(app.map_sel));
        }
    }
    if (down & KEY_B) { app.st = ST_EXTRAS; return; }
    if (app.map_sel < app.map_scroll) app.map_scroll = app.map_sel;
    if (app.map_sel >= app.map_scroll + MAP_ROWS) app.map_scroll = app.map_sel - MAP_ROWS + 1;
    if (app.toast_frames > 0) app.toast_frames--;

    int ms = map_show_state(app.map_sel);
    bool team = fe_map_encounter(s, app.map_sel) == FE_ENC_TEAM;
    const u32 colors[5] = {CLR_DIM, CLR_ACCENT, CLR_GOOD, CLR_ERR, CLR_WARN};
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "World map & chapters%s", app.dirty ? "  * unsaved" : "");
    ui_text_right(TOP_W - 8, 3, 0.5f, CLR_TEXT, "Story: %s", fe_chapter_name(fe_story_chapter(s)));
    ui_text(12, 32, 0.65f, CLR_ACCENT, "%s", fe_map_name(app.map_sel));
    ui_text(12, 56, 0.5f, CLR_TEXT, "%s", fe_map_place(app.map_sel));
    ui_text(12, 76, 0.5f, colors[ms], "%s%s", MS_NAMES[ms], team ? "   (+ StreetPass/SpotPass team)" : "");
    ui_text_wrap(12, 100, 0.44f, CLR_DIM, TOP_W - 24,
                 "Locked: hidden. Open: playable (replay a chapter). Beaten: cleared. Risen: a skirmish to "
                 "fight. Merchant: a traveling shop. Story here: continue the story from this chapter.");
    ui_text_wrap(12, 164, 0.44f, CLR_WARN, TOP_W - 24, "Experimental: test on a copy of your save first.");
    if (app.toast_frames > 0) ui_text(12, 188, 0.48f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 222, 0.42f, CLR_DIM, "A/Left/Right: change  L/R: page  X: set all  SELECT: story here");

    ui_bottom();
    for (int r = 0; r < MAP_ROWS && app.map_scroll + r < n; r++) {
        int loc = app.map_scroll + r;
        float y = 2 + r * 24.0f;
        if (loc == app.map_sel) ui_rect(0, y - 1, BOTTOM_W, 24, CLR_SEL);
        int ls = map_show_state(loc);
        ui_text(8, y + 3, 0.48f, ls == MS_LOCKED ? CLR_DIM : CLR_TEXT, "%s", fe_map_name(loc));
        ui_text(110, y + 4, 0.42f, CLR_DIM, "%.20s", fe_map_place(loc));
        ui_text_right(BOTTOM_W - 6, y + 3, 0.46f, colors[ls], "%s", MS_NAMES[ls]);
    }
    ui_button_draw(&BTN_MP_ALL, true);
    ui_button_draw(&BTN_MP_STORY, story_loc);
    ui_button_draw(&BTN_MP_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Import an Avatar
//---------------------------------------------------------------------------
#define IMPORT_MAX 160
#define CP_ROOT "sdmc:/3ds/Checkpoint/saves"
static const char *const IMP_NAMES[4] = {"From a save file", "From the logbook", "From Checkpoint",
                                         "From the hero list"};
static const char *const IMP_INFO[4] = {
    "Avatars in your save slots: each slot's army (Avatars and logbook units) and the StreetPass / "
    "SpotPass teams on its world map.",
    "Avatars in your logbook (shared Global file): StreetPass and SpotPass units you have met.",
    "Avatars in the Checkpoint backups on the SD card (" CP_ROOT "/0x00A05 ...).",
    "The 138 SpotPass and DLC heroes (Marth, Ike, Lyn, Roy...) with their class, level, stats, "
    "skills, items and look. DLC versions join at level 1."};
static const ui_button BTN_IM_BACK = {234, 202, 82, 34, "Back"};

static void imp_preview_free(void)
{
    if (app.imp_prev) {
        fe_save_free(app.imp_prev);
        free(app.imp_prev);
        app.imp_prev = NULL;
    }
    app.imp_prev_for = -1;
}

static int gather_file(const u8 *buf, u32 size, const char *prefix, bool skip_army, int room)
{
    fe_save *t = malloc(sizeof(fe_save));
    char err[160];
    int n = 0;
    if (t && fe_save_load(t, buf, size, err, sizeof(err))) {
        n = fe_import_gather_save(t, prefix, skip_army, app.imp + app.imp_count, room);
        fe_save_free(t);
    } else if (t) {
        LOGW("import: %s not readable: %s", prefix, err);
    }
    free(t);
    return n;
}

static int gather_checkpoint(void)
{
    char want[16];
    snprintf(want, sizeof(want), "0x%05llX", (unsigned long long)((app.title->title_id >> 8) & 0xFFFFF));
    DIR *root = opendir(CP_ROOT);
    if (!root) return -1;
    char game[160] = "";
    struct dirent *e;
    while ((e = readdir(root))) {
        if (strncasecmp(e->d_name, want, strlen(want)) == 0) {
            snprintf(game, sizeof(game), CP_ROOT "/%.120s", e->d_name);
            break;
        }
    }
    closedir(root);
    if (!game[0]) return -1;
    DIR *d = opendir(game);
    if (!d) return -1;
    int found = 0;
    while ((e = readdir(d)) && app.imp_count < IMPORT_MAX) {
        if (e->d_name[0] == '.') continue;
        for (int slot = 0; slot < SLOT_COUNT && app.imp_count < IMPORT_MAX; slot++) {
            char path[420];
            snprintf(path, sizeof(path), "%s/%.200s/Chapter%d", game, e->d_name, slot);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            u8 *buf = sz > 0 && sz < 0x100000 ? malloc((size_t)sz) : NULL;
            bool ok = buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz;
            fclose(f);
            if (ok) {
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "%.40s S%d", e->d_name, slot + 1);
                app.imp_count += gather_file(buf, (u32)sz, prefix, false, IMPORT_MAX - app.imp_count);
                found++;
            }
            free(buf);
        }
    }
    closedir(d);
    return found;
}

static void open_import(int src)
{
    if (!app.imp) app.imp = malloc(sizeof(fe_import_cand) * IMPORT_MAX);
    if (!app.imp) { toast("Out of memory"); return; }
    imp_preview_free();
    app.imp_count = app.imp_sel = app.imp_scroll = 0;
    app.imp_armed = -1;
    ui_busy("Looking for Avatars", IMP_NAMES[src]);
    if (src == 0) {
        for (int slot = 0; slot < SLOT_COUNT && app.imp_count < IMPORT_MAX; slot++) {
            char prefix[24];
            if (slot == app.slot) {
                snprintf(prefix, sizeof(prefix), "Slot %d (this)", slot + 1);
                app.imp_count += fe_import_gather_save(app.save, prefix, true, app.imp + app.imp_count,
                                                       IMPORT_MAX - app.imp_count);
                continue;
            }
            if (!app.slots[slot].ok) continue;
            char name[16];
            slot_file(slot, name, sizeof(name));
            u8 *buf;
            u32 size;
            if (R_FAILED(fs_read_file(app.arch, name, &buf, &size))) continue;
            snprintf(prefix, sizeof(prefix), "Slot %d", slot + 1);
            app.imp_count += gather_file(buf, size, prefix, false, IMPORT_MAX - app.imp_count);
            free(buf);
        }
    } else if (src == 3) {
        app.imp_count = fe_import_gather_heroes(app.imp, IMPORT_MAX);
    } else if (src == 1) {
        if (!global_ensure_loaded()) return;
        app.imp_count = fe_import_gather_logbook(app.global.data, app.global.size, app.imp, IMPORT_MAX);
    } else {
        int r = gather_checkpoint();
        if (r < 0) {
            show_message("No Checkpoint backups", CLR_WARN, ST_IMPORT_MENU,
                         "No Fire Emblem Awakening folder was found in " CP_ROOT ".");
            return;
        }
    }
    LOGI("import: %d Avatar(s) found (%s)", app.imp_count, IMP_NAMES[src]);
    if (!app.imp_count) {
        show_message("No Avatars found", CLR_WARN, ST_IMPORT_MENU, "%s: no Avatars to import.", IMP_NAMES[src]);
        return;
    }
    app.toast_frames = 0;
    app.st = ST_IMPORT_PICK;
}

static void screen_import_menu(u32 down, u32 repeat, const touchPosition *touch)
{
    if (repeat & KEY_UP) app.imp_menu_sel = (app.imp_menu_sel + 3) % 4;
    if (repeat & KEY_DOWN) app.imp_menu_sel = (app.imp_menu_sel + 1) % 4;
    if (down & KEY_TOUCH) {
        for (int i = 0; i < 4; i++) {
            ui_button b = {10, 8 + i * 48.0f, 300, 42, NULL};
            if (ui_button_hit(&b, touch)) { app.imp_menu_sel = i; down |= KEY_A; }
        }
        if (ui_button_hit(&BTN_IM_BACK, touch)) down |= KEY_B;
    }
    if (down & KEY_A) { open_import(app.imp_menu_sel); return; }
    if (down & KEY_B) { app.st = ST_ADD_UNIT; return; }

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Import an Avatar");
    ui_text(12, 36, 0.62f, CLR_ACCENT, "%s", IMP_NAMES[app.imp_menu_sel]);
    ui_text_wrap(12, 64, 0.48f, CLR_TEXT, TOP_W - 24, IMP_INFO[app.imp_menu_sel]);
    ui_text_wrap(12, 140, 0.44f, CLR_DIM, TOP_W - 24,
                 "With supports on, it joins as (M)/(F) Robin by its gender and can build supports. "
                 "With supports off, it joins as a logbook unit. Toggle on the next screen (X).");
    ui_text(8, 222, 0.42f, CLR_DIM, "A: open   B: back");
    ui_bottom();
    for (int i = 0; i < 4; i++) {
        ui_button b = {10, 8 + i * 48.0f, 300, 42, IMP_NAMES[i]};
        if (i == app.imp_menu_sel) ui_rect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, CLR_SEL);
        ui_button_draw(&b, true);
    }
    ui_button_draw(&BTN_IM_BACK, true);
    ui_end();
}

#define IMP_ROWS 8
static const ui_button BTN_IP_PREV = {4, 202, 40, 34, "<<"};
static const ui_button BTN_IP_NEXT = {46, 202, 40, 34, ">>"};
static const ui_button BTN_IP_SUP  = {88, 202, 74, 34, NULL};
static const ui_button BTN_IP_ADD  = {164, 202, 66, 34, "Import"};

static void imp_make_preview(void)
{
    if (app.imp_prev_for == app.imp_sel) return;
    imp_preview_free();
    fe_save *t = malloc(sizeof(fe_save));
    if (!t || !fe_save_clone(app.save, t)) { free(t); return; }
    char err[160];
    int ni = fe_import_add(t, &app.imp[app.imp_sel], !app.imp_no_supports, err, sizeof(err));
    if (ni < 0) {
        LOGW("import preview %s failed: %s", app.imp[app.imp_sel].name, err);
        fe_save_free(t);
        free(t);
        app.imp_prev_for = app.imp_sel;
        return;
    }
    app.imp_prev = t;
    app.imp_prev_unit = ni;
    app.imp_prev_for = app.imp_sel;
}

static void screen_import_pick(u32 down, u32 repeat, const touchPosition *touch)
{
    int n = app.imp_count, before = app.imp_sel;
    if (repeat & KEY_UP) app.imp_sel = (app.imp_sel + n - 1) % n;
    if (repeat & KEY_DOWN) app.imp_sel = (app.imp_sel + 1) % n;
    if (repeat & (KEY_L | KEY_LEFT)) app.imp_sel = app.imp_sel >= IMP_ROWS ? app.imp_sel - IMP_ROWS : 0;
    if (repeat & (KEY_R | KEY_RIGHT)) app.imp_sel = app.imp_sel + IMP_ROWS < n ? app.imp_sel + IMP_ROWS : n - 1;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < IMP_ROWS && app.imp_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 24.0f, BOTTOM_W, 23, NULL};
            if (ui_button_hit(&row, touch)) app.imp_sel = app.imp_scroll + r;
        }
        if (ui_button_hit(&BTN_IP_PREV, touch)) app.imp_sel = app.imp_scroll >= IMP_ROWS ? app.imp_scroll - IMP_ROWS : 0;
        if (ui_button_hit(&BTN_IP_NEXT, touch)) app.imp_sel = app.imp_scroll + IMP_ROWS < n ? app.imp_scroll + IMP_ROWS : n - 1;
        if (ui_button_hit(&BTN_IP_ADD, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_IP_SUP, touch)) down |= KEY_X;
        if (ui_button_hit(&BTN_IM_BACK, touch)) down |= KEY_B;
    }
    if (down & KEY_X) {
        app.imp_no_supports = !app.imp_no_supports;
        imp_preview_free();
        app.imp_armed = -1;
        toast("Supports %s: joins as %s", app.imp_no_supports ? "off" : "on",
              app.imp_no_supports ? "a logbook unit" : "(M)/(F) Robin");
    }
    if (app.imp_sel != before) app.imp_armed = -1;
    if (app.imp_sel < app.imp_scroll) app.imp_scroll = app.imp_sel;
    if (app.imp_sel >= app.imp_scroll + IMP_ROWS) app.imp_scroll = app.imp_sel - IMP_ROWS + 1;
    const fe_import_cand *c = &app.imp[app.imp_sel];

    if (down & KEY_A) {
        if (app.imp_armed != app.imp_sel) {
            app.imp_armed = app.imp_sel;
            toast("Press A again to import %s", c->name);
        } else {
            app.imp_armed = -1;
            char err[160];
            ui_busy("Importing", c->name);
            int ni = fe_import_add(app.save, c, !app.imp_no_supports, err, sizeof(err));
            if (ni < 0) {
                LOGE("import %s (%s) failed: %s", c->name, c->source, err);
                toast("Could not import: %s", err);
            } else {
                app.dirty = true;
                build_visible_list();
                for (int v = 0; v < app.vis_count; v++)
                    if (app.visible[v] == ni) app.unit_sel = v;
                if (app.unit_sel >= LIST_ROWS) app.unit_scroll = app.unit_sel - LIST_ROWS + 1;
                LOGI("imported %s from %s as %s", c->name, c->source,
                     fe_unit_char_id(app.save, &app.save->units[ni]) ? "(F) Robin" : "(M) Robin");
                toast("%s joined the army", c->name);
            }
        }
    }
    if (down & KEY_B) { imp_preview_free(); app.imp_armed = -1; app.st = ST_IMPORT_MENU; return; }
    if (app.toast_frames > 0) app.toast_frames--;
    imp_make_preview();

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Import an Avatar%s", app.dirty ? "  * unsaved" : "");
    ui_text_right(TOP_W - 8, 3, 0.45f, CLR_DIM, "%d/%d", app.imp_sel + 1, n);
    ui_text(12, 28, 0.65f, CLR_ACCENT, "%s", c->name);
    ui_text_right(TOP_W - 12, 32, 0.42f, CLR_DIM, "%.40s", c->source);
    fe_save *t = app.imp_prev;
    if (t) {
        const fe_unit *u = &t->units[app.imp_prev_unit];
        bool female = fe_unit_char_id(t, u) == 1;
        int jc = fe_unit_char_id(t, u);
        ui_text(12, 52, 0.5f, CLR_TEXT, "%s  Lv %d   %s   Joins as %s", fe_class_name(fe_unit_class_id(t, u)),
                fe_unit_level(t, u), fe_look_get(t, u, FE_LOOK_GENDER) ? "Female" : "Male",
                jc == 0 ? "(M) Robin" : jc == 1 ? "(F) Robin" : "logbook unit");
        (void)female;
        fe_stats st;
        if (fe_unit_stats(t, u, &st))
            for (int i = 0; i < FE_STAT_COUNT; i++) {
                float x = 12 + (i % 4) * 96, y = 72 + (i / 4) * 18;
                ui_text(x, y, 0.5f, CLR_DIM, "%s", fe_stat_names[i]);
                ui_text(x + 34, y, 0.5f, st.value[i] >= st.cap[i] ? CLR_GOOD : CLR_TEXT, "%d", fe_stat_shown(&st, i));
            }
        int eq[FE_EQUIP_SLOTS];
        fe_unit_equipped(t, u, eq);
        char line[200] = "";
        size_t o = 0;
        for (int k = 0; k < FE_EQUIP_SLOTS; k++)
            if (eq[k] && eq[k] < FE_SKILL_COUNT && o < sizeof(line))
                o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%s", o ? ", " : "", fe_skills[eq[k]].name);
        char txt[220];
        snprintf(txt, sizeof(txt), "Skills: %s", o ? line : "(none)");
        ui_text_wrap(12, 112, 0.44f, CLR_TEXT, TOP_W - 24, txt);
        o = 0;
        line[0] = '\0';
        for (int k = 0; k < 5 && o < sizeof(line); k++) {
            fe_inv_item it;
            fe_inv_get(t, u, k, &it);
            if (!it.id) continue;
            char nm[32];
            fe_item_name(t, it.id, nm, sizeof(nm));
            o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%s", o ? ", " : "", nm);
        }
        snprintf(txt, sizeof(txt), "Items: %s", o ? line : "(none)");
        ui_text_wrap(12, 146, 0.44f, CLR_TEXT, TOP_W - 24, txt);
        ui_text(12, 180, 0.44f, CLR_DIM, "Weapon ranks: %s %s %s %s %s %s  (Sw La Ax Bo To St)",
                fe_wexp_rank(fe_unit_wexp(t, u, 0)), fe_wexp_rank(fe_unit_wexp(t, u, 1)), fe_wexp_rank(fe_unit_wexp(t, u, 2)),
                fe_wexp_rank(fe_unit_wexp(t, u, 3)), fe_wexp_rank(fe_unit_wexp(t, u, 4)), fe_wexp_rank(fe_unit_wexp(t, u, 5)));
        int cid = fe_unit_char_id(t, u), ns = 0;
        const fe_support_list *csl = fe_char_supports(cid);
        for (int k = 0; csl && k < csl->count; k++)
            if (csl->partners[k].partner != FE_SUPPORT_EMPTY)
                ns += fe_support_get(t, u, csl->partners[k].partner) > 0;
        int src_char = app.imp[app.imp_sel].kind == FE_IMPORT_DU
                           ? (app.imp[app.imp_sel].data[1] | app.imp[app.imp_sel].data[2] << 8) : -1;
        if (src_char > 2 && !app.imp_no_supports)
            ui_text(12, 196, 0.44f, CLR_WARN, "Own 3D head and hair only with supports off (X)");
        else if (app.imp_no_supports) ui_text(12, 196, 0.44f, CLR_DIM, "Supports off: no supports");
        else ui_text(12, 196, 0.44f, CLR_DIM, "Supports on. Kept: %d (S lowered to A)", ns);
    } else {
        ui_text(12, 60, 0.5f, CLR_WARN, "This Avatar can't be imported into this save (see debug.log).");
    }
    if (app.toast_frames > 0) ui_text(12, 208, 0.46f, CLR_GOOD, "%s", app.toast);
    else ui_text(8, 222, 0.42f, CLR_DIM, "A, A: import   X: supports on/off   L/R: page   B: back");

    ui_bottom();
    for (int r = 0; r < IMP_ROWS && app.imp_scroll + r < n; r++) {
        int k = app.imp_scroll + r;
        float y = 2 + r * 24.0f;
        if (k == app.imp_sel) ui_rect(0, y - 1, BOTTOM_W, 24, k == app.imp_armed ? CLR_ACCENT : CLR_SEL);
        const char *src = app.imp[k].source;
        bool dlc = !strcmp(src, "DLC"), spot = !strcmp(src, "SpotPass");
        ui_text(8, y + 3, 0.48f, CLR_TEXT, "%s", app.imp[k].name);
        ui_text_right(BOTTOM_W - 6, y + 4, dlc || spot ? 0.46f : 0.4f, dlc ? CLR_WARN : spot ? CLR_GOOD : CLR_DIM, "%.34s", src);
    }
    ui_button_draw(&BTN_IP_PREV, app.imp_scroll > 0);
    ui_button_draw(&BTN_IP_NEXT, app.imp_scroll + IMP_ROWS < n);
    ui_button sp = BTN_IP_SUP;
    sp.label = app.imp_no_supports ? "Supp: Off" : "Supp: On";
    ui_button_draw(&sp, true);
    ui_button b = BTN_IP_ADD;
    if (app.imp_armed == app.imp_sel) b.label = "Confirm";
    ui_button_draw(&b, t != NULL);
    ui_button_draw(&BTN_IM_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Parents of a child unit
//---------------------------------------------------------------------------
static const ui_button BTN_PA_DONE = {234, 202, 82, 34, "Done"};

static const char *parent_label(int id)
{
    return id < 0 ? "(none)" : fe_char(id) ? fe_char(id)->name : "?";
}

static void parent_step(const fe_unit *u, int side, int dir)
{
    // candidates: none, then every character except the logbook unit (2)
    int cur = fe_child_parent(app.save, u, side), next = cur;
    do {
        next += dir;
        if (next < -1) next = fe_char_count() - 1;
        if (next >= fe_char_count()) next = -1;
    } while (next >= 0 && fe_char_vanilla_id(next) == 2);
    fe_child_set_parent(app.save, u, side, next);
    app.dirty = true;
    char name[48];
    fe_unit_name(app.save, u, name, sizeof(name));
    LOGI("parents %s: %s %s -> %s", name, side ? "mother" : "father", parent_label(cur), parent_label(next));
}

static void child_hair_type(const fe_unit *u)
{
    char cur[8], buf[16];
    snprintf(cur, sizeof(cur), "%06lX", (unsigned long)fe_unit_hair(app.save, u));
    if (!ask_text("Hair color as hex RRGGBB", cur, 6, buf, sizeof(buf))) return;
    char *end;
    unsigned long v = strtoul(buf, &end, 16);
    if (*end != '\0' || strlen(buf) != 6) { toast("Use 6 hex digits, e.g. 325AB4"); return; }
    fe_unit_set_hair(app.save, u, (uint32_t)v);
    app.dirty = true;
    LOGI("child hair color -> %06lX", v);
}

static void child_hair_preset(const fe_unit *u, int dir)
{
    uint32_t cur = fe_unit_hair(app.save, u);
    int k = -1;
    for (int i = 0; i < HAIR_PRESET_COUNT; i++)
        if (HAIR_PRESETS[i] == cur) k = i;
    k = k < 0 ? 0 : (k + HAIR_PRESET_COUNT + dir) % HAIR_PRESET_COUNT;
    fe_unit_set_hair(app.save, u, HAIR_PRESETS[k]);
    app.dirty = true;
}

static const ui_button BTN_PA_DEFHAIR = {4, 202, 110, 34, "Default hair"};
static const ui_button BTN_PA_SKILLS  = {118, 202, 112, 34, "Inherit skills"};

static void screen_parents(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    const fe_unit *u = selected_unit();
    if (!u || u->child_off < 0) { app.st = ST_MORE; return; }
    if (repeat & KEY_UP) app.par_row = (app.par_row + 2) % 3;
    if (repeat & KEY_DOWN) app.par_row = (app.par_row + 1) % 3;
    int dir = (repeat & KEY_LEFT) ? -1 : (repeat & KEY_RIGHT) ? 1 : 0;
    int big = (repeat & KEY_L) ? -10 : (repeat & KEY_R) ? 10 : 0;
    if (app.par_row < 2) {
        if (dir) parent_step(u, app.par_row, dir);
        for (int k = 0; k < (big < 0 ? -big : big); k++) parent_step(u, app.par_row, big < 0 ? -1 : 1);
        if (down & KEY_X) { fe_child_set_parent(s, u, app.par_row, -1); app.dirty = true; }
    } else {
        if (dir) child_hair_preset(u, dir);
        if (down & KEY_A) { child_hair_type(u); down &= ~(u32)KEY_A; }
    }
    bool def_hair = (down & KEY_START) != 0, inherit = (down & KEY_SELECT) != 0;
    if (down & KEY_TOUCH) {
        for (int side = 0; side < 2; side++) {
            float y = 6 + side * 62.0f;
            ui_button row = {0, y, 146, 56, NULL};
            ui_button minus = {150, y + 18, 70, 34, NULL}, plus = {230, y + 18, 80, 34, NULL};
            if (ui_button_hit(&row, touch)) app.par_row = side;
            if (ui_button_hit(&minus, touch)) { app.par_row = side; parent_step(u, side, -1); }
            if (ui_button_hit(&plus, touch)) { app.par_row = side; parent_step(u, side, +1); }
        }
        ui_button hair = {0, 130, BOTTOM_W, 64, NULL};
        if (ui_button_hit(&hair, touch)) { app.par_row = 2; child_hair_type(u); }
        if (ui_button_hit(&BTN_PA_DEFHAIR, touch)) def_hair = true;
        if (ui_button_hit(&BTN_PA_SKILLS, touch)) inherit = true;
        if (ui_button_hit(&BTN_PA_DONE, touch)) down |= KEY_B;
    }
    char name[48];
    fe_unit_name(s, u, name, sizeof(name));
    if (def_hair) {
        uint32_t h = fe_child_default_hair(s, u);
        fe_unit_set_hair(s, u, h);
        app.dirty = true;
        LOGI("child %s: default hair %06lX", name, (unsigned long)h);
        toast("Hair set to the default (#%06lX)", (unsigned long)h);
    }
    if (inherit) {
        int n = fe_child_inherit_skills(s, u);
        if (n) app.dirty = true;
        LOGI("child %s: inherited %d skills from the parents", name, n);
        if (n) toast("Learned %d skill%s from the parents (equip them in Skills)", n, n == 1 ? "" : "s");
        else toast("Nothing new: the parents have no skills this unit lacks");
    }
    if (down & (KEY_B | KEY_A)) { app.st = ST_MORE; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    uint32_t hair = fe_unit_hair(s, u), defh = fe_child_default_hair(s, u);
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s - Parents%s", name, app.dirty ? "  * unsaved" : "");
    for (int side = 0; side < 2; side++) {
        float y = 28 + side * 40;
        int id = fe_child_parent(s, u, side);
        if (side == app.par_row) ui_rect(4, y - 2, TOP_W - 8, 38, CLR_SEL);
        ui_text(12, y, 0.55f, CLR_DIM, "%s", side ? "Mother" : "Father");
        ui_text(100, y, 0.6f, id < 0 ? CLR_DIM : CLR_TEXT, "%s", parent_label(id));
        bool here = id >= 0 && fe_find_unit(s, id) >= 0;
        ui_text(100, y + 19, 0.42f, here ? CLR_GOOD : CLR_DIM, "%s", id < 0 ? "" : here ? "in this save" : "not in this save");
    }
    float hy = 110;
    if (app.par_row == 2) ui_rect(4, hy - 2, TOP_W - 8, 22, CLR_SEL);
    ui_text(12, hy, 0.55f, CLR_DIM, "Hair");
    ui_rect(100, hy + 1, 40, 16, C2D_Color32((hair >> 16) & 0xFF, (hair >> 8) & 0xFF, hair & 0xFF, 0xFF));
    ui_text(148, hy, 0.5f, CLR_TEXT, "#%06lX", (unsigned long)hair);
    ui_text(230, hy + 2, 0.42f, hair == defh ? CLR_GOOD : CLR_DIM, "%s (#%06lX)", hair == defh ? "default" : "default is",
            (unsigned long)defh);
    fe_stats st;
    if (fe_unit_stats(s, u, &st)) {
        ui_text(12, 136, 0.44f, CLR_DIM, "Stat caps with these parents:");
        for (int i = 0; i < FE_STAT_COUNT; i++) {
            float x = 12 + (i % 4) * 96, y = 152 + (i / 4) * 17;
            ui_text(x, y, 0.46f, CLR_DIM, "%s", fe_stat_names[i]);
            ui_text(x + 34, y, 0.46f, CLR_TEXT, "%d", st.cap[i]);
        }
    }
    if (app.toast_frames > 0)
        ui_text(8, 206, 0.44f, CLR_GOOD, "%s", app.toast);
    else
        ui_text(8, 206, 0.42f, CLR_DIM, "Asset/flaw and grandparents come from that parent in this save.");
    ui_text(8, 222, 0.42f, CLR_DIM, "Left/Right: change  Hair: A type hex  START: default  SEL: skills");

    ui_bottom();
    for (int side = 0; side < 2; side++) {
        float y = 6 + side * 62.0f;
        int id = fe_child_parent(s, u, side);
        if (side == app.par_row) ui_rect(0, y - 2, BOTTOM_W, 58, CLR_SEL);
        ui_text(10, y, 0.48f, CLR_DIM, "%s", side ? "Mother" : "Father");
        ui_text(10, y + 24, 0.56f, CLR_TEXT, "%s", parent_label(id));
        ui_button minus = {150, y + 18, 70, 34, "<"}, plus = {230, y + 18, 80, 34, ">"};
        ui_button_draw(&minus, true);
        ui_button_draw(&plus, true);
    }
    if (app.par_row == 2) ui_rect(0, 128, BOTTOM_W, 66, CLR_SEL);
    ui_text(10, 132, 0.48f, CLR_DIM, "Hair color (tap to type hex)");
    ui_rect(10, 154, 60, 30, C2D_Color32((hair >> 16) & 0xFF, (hair >> 8) & 0xFF, hair & 0xFF, 0xFF));
    ui_text(80, 160, 0.56f, CLR_TEXT, "#%06lX", (unsigned long)hair);
    ui_button_draw(&BTN_PA_DEFHAIR, true);
    ui_button_draw(&BTN_PA_SKILLS, true);
    ui_button_draw(&BTN_PA_DONE, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Backups: restore / delete
//---------------------------------------------------------------------------
#define BK_ROWS 6
static const ui_button BTN_BK_AUTO = {4, 170, 312, 28, NULL};
static const ui_button BTN_BK_RESTORE = {4, 202, 74, 34, "Restore"};
static const ui_button BTN_BK_DELETE  = {82, 202, 70, 34, "Delete"};
static const ui_button BTN_BK_ALL     = {156, 202, 74, 34, "Del. all"};
static const ui_button BTN_BK_BACK    = {234, 202, 82, 34, "Back"};

static void open_backups(void)
{
    if (!app.bk) app.bk = malloc(sizeof(fs_backup) * FS_MAX_BACKUPS);
    if (!app.bk) return;
    app.bk_count = fs_list_backups(app.bk, FS_MAX_BACKUPS);
    if (app.bk_sel >= app.bk_count) app.bk_sel = app.bk_count ? app.bk_count - 1 : 0;
    app.bk_armed = 0;
    app.toast_frames = 0;
    app.st = ST_BACKUPS;
}

static void screen_backups(u32 down, u32 repeat, const touchPosition *touch)
{
    int n = app.bk_count, before = app.bk_sel, action = 0;
    if (app.bk_combo > 0) {  // Delete all: Up, Down, Left, Right, B, A; any other button cancels
        static const u32 SEQ[6] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_B, KEY_A};
        u32 keys = down & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L |
                           KEY_R | KEY_ZL | KEY_ZR | KEY_START | KEY_SELECT | KEY_TOUCH);
        if (keys) {
            u32 want = SEQ[app.bk_combo - 1];
            if ((keys & want) && !(keys & ~want)) {
                app.bk_combo++;
            } else {
                app.bk_combo = 0;
                toast("Delete all cancelled");
            }
            if (app.bk_combo > 6) {
                app.bk_combo = 0;
                int d = fs_delete_all_backups();
                LOGI("deleted all backups (%d)", d);
                toast("Deleted %d backup%s", d, d == 1 ? "" : "s");
                app.bk_count = fs_list_backups(app.bk, FS_MAX_BACKUPS);
                app.bk_sel = 0;
                n = app.bk_count;
            }
        }
        down = 0;
        repeat = 0;
    }
    if (n && (repeat & KEY_UP)) app.bk_sel = (app.bk_sel + n - 1) % n;
    if (n && (repeat & KEY_DOWN)) app.bk_sel = (app.bk_sel + 1) % n;
    if (down & KEY_A) action = 1;
    if (down & KEY_X) action = 2;
    if (down & KEY_Y) action = 3;
    if ((down & KEY_TOUCH) && ui_button_hit(&BTN_BK_AUTO, touch)) down |= KEY_START;
    if (down & KEY_START) {
        app.auto_backup = !app.auto_backup;
        settings_save();
        toast("Backup before saving: %s", app.auto_backup ? "ON" : "OFF");
    }
    if (down & KEY_TOUCH) {
        for (int r = 0; r < BK_ROWS && app.bk_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 28.0f, BOTTOM_W, 27, NULL};
            if (ui_button_hit(&row, touch)) app.bk_sel = app.bk_scroll + r;
        }
        if (ui_button_hit(&BTN_BK_RESTORE, touch)) action = 1;
        if (ui_button_hit(&BTN_BK_DELETE, touch)) action = 2;
        if (ui_button_hit(&BTN_BK_ALL, touch)) action = 3;
        if (ui_button_hit(&BTN_BK_BACK, touch)) down |= KEY_B;
    }
    if (app.bk_sel != before) app.bk_armed = 0;
    if (app.bk_sel < app.bk_scroll) app.bk_scroll = app.bk_sel;
    if (app.bk_sel >= app.bk_scroll + BK_ROWS) app.bk_scroll = app.bk_sel - BK_ROWS + 1;
    if (action && n) {
        const fs_backup *b = &app.bk[app.bk_sel];
        if (action == 3) {
            app.bk_armed = 0;
            app.bk_combo = 1;
        } else if (app.bk_armed != action) {
            app.bk_armed = action;
            toast(action == 1 ? "Press again to restore %s" : "Press again to delete %s", b->name);
        } else {
            app.bk_armed = 0;
            if (action == 1) {
                char err[200], dir[160], name[32];
                snprintf(name, sizeof(name), "%s", b->name);
                ui_busy("Restoring", "Backing up the current save first...");
                if (!fs_backup_save(app.arch, dir, sizeof(dir), err, sizeof(err))) {
                    show_message("Restore cancelled", CLR_ERR, ST_BACKUPS, "Could not back up the current save first:\n%s", err);
                    return;
                }
                ui_busy("Restoring", name);
                bool ok = fs_restore_backup(app.arch, name, err, sizeof(err));
                int ti = (int)(app.title - app.titles);
                open_title(ti);  // reopen and rescan the slots
                if (app.st == ST_FATAL) return;
                if (ok)
                    show_message("Backup restored", CLR_GOOD, ST_PICK_SLOT,
                                 "The save is now %s.\n\nThe previous save was backed up to\n%s", name, dir);
                else
                    show_message("Restore failed", CLR_ERR, ST_PICK_SLOT, "%s\n\nThe save before the restore is in\n%s", err, dir);
                return;
            }
            if (action == 2) {
                bool ok = fs_delete_backup(b->name);
                toast(ok ? "Deleted %s" : "Could not delete %s", b->name);
            }
            app.bk_count = fs_list_backups(app.bk, FS_MAX_BACKUPS);
            if (app.bk_sel >= app.bk_count) app.bk_sel = app.bk_count ? app.bk_count - 1 : 0;
            n = app.bk_count;
        }
    }
    if (down & KEY_B) { app.bk_armed = 0; app.st = ST_PICK_SLOT; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Backups");
    ui_text_right(TOP_W - 8, 3, 0.45f, CLR_DIM, "%d in sdmc:/3ds/FEAEditor/backups", n);
    if (app.bk_combo > 0) {
        static const char *const STEP[6] = {"Up", "Down", "Left", "Right", "B", "A"};
        ui_text(16, 40, 0.75f, CLR_ERR, "Delete ALL %d backups?", n);
        ui_text_wrap(16, 76, 0.5f, CLR_TEXT, TOP_W - 32, "This removes every backup in sdmc:/3ds/FEAEditor/backups.");
        for (int i = 0; i < 6; i++)
            ui_text(24 + i * 60, 120, 0.7f, i < app.bk_combo - 1 ? CLR_GOOD : CLR_TEXT, "%s", STEP[i]);
        ui_text(16, 170, 0.48f, CLR_DIM, "Press these in order. Any other button cancels.");
    } else if (n) {
        const fs_backup *b = &app.bk[app.bk_sel];
        ui_text(12, 32, 0.62f, CLR_ACCENT, "%s", b->name);
        ui_text(12, 56, 0.5f, CLR_TEXT, "%d files, %lu KB", b->files, (unsigned long)((b->bytes + 1023) / 1024));
    } else {
        ui_text(12, 40, 0.55f, CLR_DIM, app.auto_backup ? "No backups yet. One is made every time you save." : "No backups yet.");
    }
    if (!app.bk_combo) {
        ui_text_wrap(12, 84, 0.44f, CLR_DIM, TOP_W - 24,
                     "Restore makes the whole save (all slots and Global) exactly like the backup. "
                     "The current save is backed up first, so a restore can be undone.");
        ui_text(8, 158, 0.46f, app.auto_backup ? CLR_GOOD : CLR_WARN, "Backup before saving: %s",
                app.auto_backup ? "ON (a full copy each time you save)" : "OFF (no copies are made)");
        ui_text(8, 206, 0.42f, CLR_DIM, "START or tap the toggle: backups on/off (remembered)");
        ui_text(8, 222, 0.42f, CLR_DIM, "A/X: restore/delete (press twice)   Y: delete all   B: back");
    }
    if (app.toast_frames > 0) ui_text(12, 186, 0.48f, app.bk_armed ? CLR_WARN : CLR_GOOD, "%s", app.toast);

    ui_bottom();
    for (int r = 0; r < BK_ROWS && app.bk_scroll + r < n; r++) {
        int k = app.bk_scroll + r;
        float y = 2 + r * 28.0f;
        if (k == app.bk_sel) ui_rect(0, y - 1, BOTTOM_W, 28, app.bk_armed ? CLR_ACCENT : CLR_SEL);
        ui_text(8, y + 5, 0.5f, CLR_TEXT, "%s", app.bk[k].name);
        ui_text_right(BOTTOM_W - 6, y + 6, 0.42f, CLR_DIM, "%d files", app.bk[k].files);
    }
    ui_button r1 = BTN_BK_RESTORE, r2 = BTN_BK_DELETE, r3 = BTN_BK_ALL;
    if (app.bk_armed == 1) r1.label = "Confirm";
    if (app.bk_armed == 2) r2.label = "Confirm";
    if (app.bk_combo > 0) r3.label = "Combo...";
    ui_button_draw(&r1, n > 0);
    ui_button_draw(&r2, n > 0);
    ui_button_draw(&r3, n > 0);
    ui_button at = BTN_BK_AUTO;
    at.label = app.auto_backup ? "Backup before saving: ON" : "Backup before saving: OFF";
    ui_rect(at.x - 2, at.y - 2, at.w + 4, at.h + 4, app.auto_backup ? CLR_GOOD : CLR_WARN);
    ui_button_draw(&at, true);
    ui_button_draw(&BTN_BK_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Forge editor
//---------------------------------------------------------------------------
enum { FG_NAME, FG_MT, FG_HIT, FG_CRIT, FG_COPIES };
static const char *const FG_LABELS[5] = {"Name", "Might +", "Hit +", "Crit +", "Copies"};
static const ui_button BTN_FG_OK     = {4, 202, 150, 34, "Forge"};
static const ui_button BTN_FG_CANCEL = {234, 202, 82, 34, "Cancel"};
#define FG_BONUS_MAX 99

static void open_forge(int mode, int slot, int item, state_t back)
{
    fe_save *s = app.save;
    memset(&app.fg, 0, sizeof(app.fg));
    app.fg.mode = mode;
    app.fg.slot = slot;
    app.fg.item = item;
    app.fg.back = back;
    app.fg.copies = 1;
    app.fg.forge = fe_forge_of_item(s, item);
    if (app.fg.forge >= 0) {
        fe_forge_name(s, app.fg.forge, app.fg.name, sizeof(app.fg.name));
        fe_forge_bonus(s, app.fg.forge, &app.fg.bonus[0], &app.fg.bonus[1], &app.fg.bonus[2]);
    } else {
        snprintf(app.fg.name, sizeof(app.fg.name), "%.17s", fe_item(item) ? fe_item(item)->name : "Forged");
        app.fg.bonus[0] = 5;
        app.fg.bonus[1] = 10;
        app.fg.bonus[2] = 5;
    }
    app.toast_frames = 0;
    app.st = ST_FORGE;
}

static void forge_apply(void)
{
    fe_save *s = app.save;
    char err[160];
    if (app.fg.forge >= 0) {
        fe_forge_edit(s, app.fg.forge, app.fg.name, app.fg.bonus[0], app.fg.bonus[1], app.fg.bonus[2]);
        app.dirty = true;
        LOGI("forge edited: %s +%d/+%d/+%d", app.fg.name, app.fg.bonus[0], app.fg.bonus[1], app.fg.bonus[2]);
        toast("%s updated (every copy of it changes)", app.fg.name);
        app.st = app.fg.back;
        return;
    }
    int base = app.fg.item;
    int id = fe_forge_create(s, base, app.fg.name, app.fg.bonus[0], app.fg.bonus[1], app.fg.bonus[2], err, sizeof(err));
    if (id < 0) {
        LOGE("forge %s failed: %s", item_name_of(base), err);
        toast("Could not forge: %s", err);
        return;
    }
    app.dirty = true;
    if (app.fg.mode == 0) {
        const fe_unit *u = selected_unit();
        fe_inv_item it;
        fe_inv_get(s, u, app.fg.slot, &it);
        fe_inv_set(s, u, app.fg.slot, id, it.uses);
        LOGI("forged %s from %s in inventory slot %d", app.fg.name, item_name_of(base), app.fg.slot + 1);
        toast("Forged %s", app.fg.name);
    } else {
        int have = fe_convoy_count_items(s, base), take = have < app.fg.copies ? have : app.fg.copies;
        fe_convoy_set_items(s, base, have - take);
        fe_convoy_set_items(s, id, app.fg.copies);
        app.list_count = 0;  // the convoy list gains the new forge
        LOGI("forged %d x %s from %s (%d taken from the convoy)", app.fg.copies, app.fg.name, item_name_of(base), take);
        toast("Forged %d x %s (%d from the convoy)", app.fg.copies, app.fg.name, take);
    }
    app.st = app.fg.back;
}

static void forge_change(int row, int delta)
{
    if (row == FG_COPIES) {
        app.fg.copies += delta;
        if (app.fg.copies < 1) app.fg.copies = 1;
        if (app.fg.copies > CONVOY_MAX_EACH) app.fg.copies = CONVOY_MAX_EACH;
    } else if (row >= FG_MT && row <= FG_CRIT) {
        int *b = &app.fg.bonus[row - FG_MT];
        *b += delta;
        if (*b < 0) *b = 0;
        if (*b > FG_BONUS_MAX) *b = FG_BONUS_MAX;
    }
}

static void forge_type(int row)
{
    if (row == FG_NAME) {
        char buf[48];
        if (ask_text("Forged weapon name (max 17)", app.fg.name, FE_FORGE_NAME_MAX, buf, sizeof(buf)) && buf[0])
            snprintf(app.fg.name, sizeof(app.fg.name), "%s", buf);
        return;
    }
    int *v = row == FG_COPIES ? &app.fg.copies : &app.fg.bonus[row - FG_MT];
    int want;
    if (ask_number(FG_LABELS[row], *v, 2, &want)) {
        *v = 0;
        forge_change(row, want);
    }
}

static void screen_forge(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    bool editing = app.fg.forge >= 0;
    int rows = (!editing && app.fg.mode == 1) ? 5 : 4;
    if (app.fg.row >= rows) app.fg.row = 0;
    if (repeat & KEY_UP) app.fg.row = (app.fg.row + rows - 1) % rows;
    if (repeat & KEY_DOWN) app.fg.row = (app.fg.row + 1) % rows;
    if (repeat & KEY_LEFT) forge_change(app.fg.row, -1);
    if (repeat & KEY_RIGHT) forge_change(app.fg.row, +1);
    if (repeat & KEY_L) forge_change(app.fg.row, -10);
    if (repeat & KEY_R) forge_change(app.fg.row, +10);
    if (down & KEY_Y) forge_type(app.fg.row);
    bool ok = (down & KEY_START) != 0;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < rows; r++) {
            float y = 6 + r * 36.0f;
            ui_button label = {0, y, 146, 32, NULL}, minus = {150, y, 50, 32, NULL}, plus = {204, y, 50, 32, NULL},
                      p10 = {258, y, 58, 32, NULL};
            if (ui_button_hit(&label, touch)) { app.fg.row = r; forge_type(r); }
            if (r != FG_NAME) {
                if (ui_button_hit(&minus, touch)) { app.fg.row = r; forge_change(r, -1); }
                if (ui_button_hit(&plus, touch)) { app.fg.row = r; forge_change(r, +1); }
                if (ui_button_hit(&p10, touch)) { app.fg.row = r; forge_change(r, +10); }
            }
        }
        if (ui_button_hit(&BTN_FG_OK, touch)) ok = true;
        if (ui_button_hit(&BTN_FG_CANCEL, touch)) down |= KEY_B;
    }
    if (down & KEY_A) { if (app.fg.row == FG_NAME) forge_type(FG_NAME); else ok = true; }
    if (ok) { forge_apply(); return; }
    if (down & KEY_B) { app.st = app.fg.back; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    int base = editing ? fe_forge_base(s, app.fg.forge) : app.fg.item;
    char bname[32];
    fe_item_name(s, base, bname, sizeof(bname));
    int mx = fe_item_max_uses(s, base);
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "%s", editing ? "Edit forged weapon" : "Forge a weapon");
    ui_text_right(TOP_W - 8, 3, 0.45f, CLR_DIM, "%d / %d forges used", s->forge_count, FE_FORGE_MAX);
    ui_text(12, 32, 0.65f, CLR_ACCENT, "%s", app.fg.name);
    ui_text(12, 58, 0.5f, CLR_TEXT, "Base: %s   %s", bname, mx ? "" : "(unbreakable)");
    if (mx) ui_text(220, 58, 0.5f, CLR_DIM, "%d uses each", mx);
    ui_text(12, 82, 0.55f, CLR_TEXT, "Might +%d   Hit +%d   Crit +%d", app.fg.bonus[0], app.fg.bonus[1], app.fg.bonus[2]);
    if (!editing && app.fg.mode == 1)
        ui_text(12, 104, 0.5f, CLR_TEXT, "Copies: %d  (takes up to %d %s from the convoy)", app.fg.copies,
                fe_convoy_count_items(s, base), item_name_of(base));
    ui_text_wrap(12, 130, 0.42f, CLR_DIM, TOP_W - 24,
                 editing ? "Changes apply to every copy of this forged weapon."
                         : "Bonuses add to the base weapon. Uses come from the base weapon, so forging an "
                           "unbreakable weapon (e.g. Falchion) keeps it unbreakable.");
    if (app.toast_frames > 0) ui_text(12, 186, 0.48f, CLR_WARN, "%s", app.toast);
    ui_text(8, 206, 0.42f, CLR_DIM, "Left/Right: 1  L/R: 10  Y or tap a name: type");
    ui_text(8, 222, 0.42f, CLR_DIM, "START: %s   B: cancel", editing ? "save changes" : "forge");

    ui_bottom();
    for (int r = 0; r < rows; r++) {
        float y = 6 + r * 36.0f;
        if (r == app.fg.row) ui_rect(0, y - 2, BOTTOM_W, 36, CLR_SEL);
        ui_text(8, y + 2, 0.46f, CLR_DIM, "%s", FG_LABELS[r]);
        if (r == FG_NAME) {
            ui_text(8, y + 16, 0.48f, CLR_TEXT, "%s", app.fg.name);
            ui_text_right(BOTTOM_W - 8, y + 10, 0.42f, CLR_DIM, "tap to type");
            continue;
        }
        int v = r == FG_COPIES ? app.fg.copies : app.fg.bonus[r - FG_MT];
        ui_text_right(140, y + 8, 0.56f, CLR_TEXT, "%d", v);
        ui_button minus = {150, y, 50, 32, "-"}, plus = {204, y, 50, 32, "+"}, p10 = {258, y, 58, 32, "+10"};
        ui_button_draw(&minus, true);
        ui_button_draw(&plus, true);
        ui_button_draw(&p10, true);
    }
    ui_button okb = BTN_FG_OK;
    if (editing) okb.label = "Save changes";
    ui_button_draw(&okb, editing || s->forge_count < FE_FORGE_MAX);
    ui_button_draw(&BTN_FG_CANCEL, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Chapter records & play time
//---------------------------------------------------------------------------
#define REC_ROWS 8
static const ui_button BTN_RC_EDIT = {120, 202, 110, 34, "Edit"};
static const ui_button BTN_RC_PREV = {4, 202, 54, 34, "<<"};
static const ui_button BTN_RC_NEXT = {62, 202, 54, 34, ">>"};
static const ui_button BTN_RC_BACK = {234, 202, 82, 34, "Back"};

static void fmt_time(char *out, size_t len, uint32_t frames, bool hours)
{
    uint32_t sec = frames / 60;
    if (hours) snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
    else snprintf(out, len, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

static void edit_playtime(void)
{
    uint32_t f = fe_playtime(app.save), sec = f / 60;
    int h, m;
    if (!ask_number("Play time: hours", (int)(sec / 3600), 4, &h)) return;
    if (!ask_number("Play time: minutes", (int)(sec / 60 % 60), 2, &m)) return;
    if (h < 0) h = 0;
    if (m < 0) m = 0;
    if (m > 59) m = 59;
    uint32_t nf = ((uint32_t)h * 3600 + (uint32_t)m * 60 + sec % 60) * 60 + f % 60;
    fe_set_playtime(app.save, nf);
    app.dirty = true;
    LOGI("play time: %lu -> %lu frames", (unsigned long)f, (unsigned long)nf);
}

static void screen_records(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    int n = fe_record_count(s) + 1;  // row 0 = total play time
    if (repeat & KEY_UP) app.rec_sel = (app.rec_sel + n - 1) % n;
    if (repeat & KEY_DOWN) app.rec_sel = (app.rec_sel + 1) % n;
    if (repeat & KEY_L) app.rec_sel = app.rec_sel >= REC_ROWS ? app.rec_sel - REC_ROWS : 0;
    if (repeat & KEY_R) app.rec_sel = app.rec_sel + REC_ROWS < n ? app.rec_sel + REC_ROWS : n - 1;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < REC_ROWS && app.rec_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 24.0f, BOTTOM_W, 23, NULL};
            if (ui_button_hit(&row, touch)) {
                if (app.rec_sel == app.rec_scroll + r) down |= KEY_A;
                app.rec_sel = app.rec_scroll + r;
            }
        }
        if (ui_button_hit(&BTN_RC_PREV, touch)) app.rec_sel = app.rec_scroll >= REC_ROWS ? app.rec_scroll - REC_ROWS : 0;
        if (ui_button_hit(&BTN_RC_NEXT, touch)) app.rec_sel = app.rec_scroll + REC_ROWS < n ? app.rec_scroll + REC_ROWS : n - 1;
        if (ui_button_hit(&BTN_RC_EDIT, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_RC_BACK, touch)) down |= KEY_B;
    }
    if (down & KEY_A) {
        if (app.rec_sel == 0) edit_playtime();
        else { app.rec_row = 0; app.toast_frames = 0; app.st = ST_RECORD_EDIT; return; }
    }
    if (down & KEY_B) { app.st = ST_EXTRAS; return; }
    if (app.rec_sel < app.rec_scroll) app.rec_scroll = app.rec_sel;
    if (app.rec_sel >= app.rec_scroll + REC_ROWS) app.rec_scroll = app.rec_sel - REC_ROWS + 1;

    char t[24];
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Records & play time%s", app.dirty ? "  * unsaved" : "");
    fmt_time(t, sizeof(t), fe_playtime(s), true);
    ui_text_right(TOP_W - 8, 3, 0.5f, CLR_TEXT, "Play time %s", t);
    if (app.rec_sel == 0) {
        ui_text(12, 36, 0.65f, CLR_ACCENT, "Total play time");
        ui_text(12, 62, 0.6f, CLR_TEXT, "%s  (h:mm:ss)", t);
        ui_text_wrap(12, 92, 0.46f, CLR_DIM, TOP_W - 24, "Shown on the file select screen. A: type hours and minutes.");
    } else {
        fe_record r;
        fe_record_get(s, app.rec_sel - 1, &r);
        fmt_time(t, sizeof(t), r.frames, false);
        ui_text(12, 36, 0.65f, CLR_ACCENT, "%s", fe_chapter_name(r.chapter));
        ui_text(12, 62, 0.55f, CLR_TEXT, "%d turns   %s", r.turns, t);
        for (int i = 0; i < 2; i++)
            ui_text(12, 88 + i * 22, 0.5f, CLR_TEXT, "Unit %d: %s  (%s)", i + 1, r.unit[i] < 0 ? "-" : char_name(r.unit[i]),
                    r.cls[i] < 0 ? "-" : fe_class_name(r.cls[i]));
        ui_text_wrap(12, 144, 0.44f, CLR_DIM, TOP_W - 24, "Records appear in the credits. A: edit this record.");
    }
    ui_text(8, 222, 0.42f, CLR_DIM, "A: edit   L/R: page   B: back");

    ui_bottom();
    for (int r = 0; r < REC_ROWS && app.rec_scroll + r < n; r++) {
        int k = app.rec_scroll + r;
        float y = 2 + r * 24.0f;
        if (k == app.rec_sel) ui_rect(0, y - 1, BOTTOM_W, 24, CLR_SEL);
        if (k == 0) {
            fmt_time(t, sizeof(t), fe_playtime(s), true);
            ui_text(8, y + 3, 0.48f, CLR_ACCENT, "Total play time");
            ui_text_right(BOTTOM_W - 6, y + 3, 0.46f, CLR_TEXT, "%s", t);
            continue;
        }
        fe_record rc;
        fe_record_get(s, k - 1, &rc);
        fmt_time(t, sizeof(t), rc.frames, false);
        ui_text(8, y + 3, 0.48f, CLR_TEXT, "%s", fe_chapter_name(rc.chapter));
        ui_text_right(BOTTOM_W - 6, y + 3, 0.44f, CLR_DIM, "%d turns  %s", rc.turns, t);
    }
    ui_button_draw(&BTN_RC_PREV, app.rec_scroll > 0);
    ui_button_draw(&BTN_RC_NEXT, app.rec_scroll + REC_ROWS < n);
    ui_button_draw(&BTN_RC_EDIT, true);
    ui_button_draw(&BTN_RC_BACK, true);
    ui_end();
}

enum { RE_TURNS, RE_TIME, RE_UNIT1, RE_CLASS1, RE_UNIT2, RE_CLASS2, RE_ROWS };
static const char *const RE_LABELS[RE_ROWS] = {"Turns", "Time", "Unit 1", "Class 1", "Unit 2", "Class 2"};

static void record_change(fe_record *r, int row, int d)
{
    if (row == RE_TURNS) { r->turns += d; if (r->turns < 0) r->turns = 0; }
    else if (row == RE_TIME) {
        int64_t f = (int64_t)r->frames + (int64_t)d * 3600;  // minutes
        r->frames = f < 0 ? 0 : (uint32_t)f;
    } else {
        int *v = row == RE_UNIT1 ? &r->unit[0] : row == RE_UNIT2 ? &r->unit[1] : row == RE_CLASS1 ? &r->cls[0] : &r->cls[1];
        int max = (row == RE_UNIT1 || row == RE_UNIT2) ? fe_char_count() : fe_class_count();
        *v += d > 0 ? 1 : -1;
        if (*v < -1) *v = max - 1;
        if (*v >= max) *v = -1;
    }
}

static void record_type(fe_record *r, int row)
{
    int v;
    if (row == RE_TURNS) {
        if (ask_number("Turns", r->turns, 4, &v)) r->turns = v < 0 ? 0 : v;
    } else if (row == RE_TIME || row == -RE_TIME) {  // RE_TIME: minutes, -RE_TIME: seconds
        uint32_t cur = r->frames / 60;
        int m = (int)(cur / 60), sec = (int)(cur % 60);
        if (row == RE_TIME) {
            if (!ask_number("Chapter time: minutes", m, 4, &v)) return;
            m = v < 0 ? 0 : v;
        } else {
            if (!ask_number("Chapter time: seconds (0-59)", sec, 2, &v)) return;
            sec = v < 0 ? 0 : v > 59 ? 59 : v;
        }
        r->frames = ((uint32_t)m * 60 + (uint32_t)sec) * 60;
    }
}

// Alphabetical unit/class picker for a record row
static const char *rp_name(bool units, int id)
{
    return id < 0 ? "(none)" : units ? char_name(id) : fe_class_name(id);
}

static bool rp_units(void) { return app.rp_row == RE_UNIT1 || app.rp_row == RE_UNIT2; }

static int rp_cmp_units(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (x < 0 || y < 0) return x - y;
    char nx[32];
    snprintf(nx, sizeof(nx), "%s", char_name(x));  // char_name may share a buffer
    return strcasecmp(nx, char_name(y));
}

static int rp_cmp_classes(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (x < 0 || y < 0) return x - y;
    return strcasecmp(fe_class_name(x), fe_class_name(y));
}

static void open_record_pick(int row, int current)
{
    app.rp_target = 0;
    app.rp_row = row;
    bool units = rp_units();
    int n = 0;
    app.rp_ids[n++] = -1;
    int max = units ? fe_char_count() : fe_class_count();
    for (int i = 0; i < max; i++) app.rp_ids[n++] = i;
    qsort(app.rp_ids + 1, (size_t)(n - 1), sizeof(int), units ? rp_cmp_units : rp_cmp_classes);
    app.rp_count = n;
    app.rp_sel = 0;
    for (int i = 0; i < n; i++)
        if (app.rp_ids[i] == current) app.rp_sel = i;
    app.rp_scroll = app.rp_sel > 3 ? app.rp_sel - 3 : 0;
    app.st = ST_RECORD_PICK;
}

#define RP_ROWS 8
static const ui_button BTN_RP_PREV = {4, 202, 54, 34, "<<"};
static const ui_button BTN_RP_NEXT = {62, 202, 54, 34, ">>"};
static const ui_button BTN_RP_PICK = {120, 202, 110, 34, "Pick"};

static void screen_record_pick(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    int k = app.rec_sel - 1, n = app.rp_count;
    bool units = rp_units();
    if (repeat & KEY_UP) app.rp_sel = (app.rp_sel + n - 1) % n;
    if (repeat & KEY_DOWN) app.rp_sel = (app.rp_sel + 1) % n;
    if (repeat & (KEY_L | KEY_LEFT)) app.rp_sel = app.rp_sel >= RP_ROWS ? app.rp_sel - RP_ROWS : 0;
    if (repeat & (KEY_R | KEY_RIGHT)) app.rp_sel = app.rp_sel + RP_ROWS < n ? app.rp_sel + RP_ROWS : n - 1;
    int pick = -2;
    if (down & KEY_A) pick = app.rp_sel;
    if (down & KEY_TOUCH) {
        for (int r = 0; r < RP_ROWS && app.rp_scroll + r < n; r++) {
            ui_button row = {0, 2 + r * 24.0f, BOTTOM_W, 23, NULL};
            if (ui_button_hit(&row, touch)) pick = app.rp_scroll + r;
        }
        if (ui_button_hit(&BTN_RP_PREV, touch)) app.rp_sel = app.rp_scroll >= RP_ROWS ? app.rp_scroll - RP_ROWS : 0;
        if (ui_button_hit(&BTN_RP_NEXT, touch)) app.rp_sel = app.rp_scroll + RP_ROWS < n ? app.rp_scroll + RP_ROWS : n - 1;
        if (ui_button_hit(&BTN_RP_PICK, touch)) pick = app.rp_sel;
        if (ui_button_hit(&BTN_RC_BACK, touch)) down |= KEY_B;
    }
    if (pick >= 0 && app.rp_target == 1) {
        fe_barracks_event ev;
        fe_barracks_get(s, app.bk_ev_sel, &ev);
        int slot = app.rp_row == RE_UNIT1 ? 0 : 1;
        ev.unit[slot] = app.rp_ids[pick];
        fe_barracks_set(s, app.bk_ev_sel, &ev);
        app.dirty = true;
        LOGI("barracks event %d: unit %d -> %s", app.bk_ev_sel + 1, slot + 1, rp_name(true, app.rp_ids[pick]));
        app.st = ST_BARRACKS;
        return;
    }
    if (pick >= 0 && k >= 0) {
        fe_record r;
        fe_record_get(s, k, &r);
        int id = app.rp_ids[pick];
        int *v = app.rp_row == RE_UNIT1 ? &r.unit[0] : app.rp_row == RE_UNIT2 ? &r.unit[1]
               : app.rp_row == RE_CLASS1 ? &r.cls[0] : &r.cls[1];
        if (*v != id) {
            *v = id;
            fe_record_set(s, k, &r);
            app.dirty = true;
            LOGI("record %d: %s -> %s", k, RE_LABELS[app.rp_row], rp_name(units, id));
        }
        app.st = ST_RECORD_EDIT;
        return;
    }
    if (down & KEY_B) { app.st = app.rp_target == 1 ? ST_BARRACKS : ST_RECORD_EDIT; return; }
    if (app.rp_sel < app.rp_scroll) app.rp_scroll = app.rp_sel;
    if (app.rp_sel >= app.rp_scroll + RP_ROWS) app.rp_scroll = app.rp_sel - RP_ROWS + 1;

    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Pick %s", RE_LABELS[app.rp_row]);
    ui_text_right(TOP_W - 8, 3, 0.45f, CLR_DIM, "%d/%d  A to Z", app.rp_sel + 1, n);
    ui_text(12, 40, 0.7f, CLR_ACCENT, "%s", rp_name(units, app.rp_ids[app.rp_sel]));
    // letter guide: first entry of each starting letter on this list
    ui_text_wrap(12, 80, 0.44f, CLR_DIM, TOP_W - 24, "Sorted alphabetically. L/R or Left/Right jump a page.");
    ui_text(8, 222, 0.42f, CLR_DIM, "Tap a name or A: pick   B: cancel");
    ui_bottom();
    for (int r = 0; r < RP_ROWS && app.rp_scroll + r < n; r++) {
        int i = app.rp_scroll + r;
        float y = 2 + r * 24.0f;
        if (i == app.rp_sel) ui_rect(0, y - 1, BOTTOM_W, 24, CLR_SEL);
        ui_text(8, y + 3, 0.5f, app.rp_ids[i] < 0 ? CLR_DIM : CLR_TEXT, "%s", rp_name(units, app.rp_ids[i]));
    }
    ui_button_draw(&BTN_RP_PREV, app.rp_scroll > 0);
    ui_button_draw(&BTN_RP_NEXT, app.rp_scroll + RP_ROWS < n);
    ui_button_draw(&BTN_RP_PICK, true);
    ui_button c = BTN_RC_BACK;
    c.label = "Cancel";
    ui_button_draw(&c, true);
    ui_end();
}

static void screen_record_edit(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    int k = app.rec_sel - 1;
    if (k < 0 || k >= fe_record_count(s)) { app.st = ST_RECORDS; return; }
    fe_record r, before;
    fe_record_get(s, k, &r);
    before = r;
    if (repeat & KEY_UP) app.rec_row = (app.rec_row + RE_ROWS - 1) % RE_ROWS;
    if (repeat & KEY_DOWN) app.rec_row = (app.rec_row + 1) % RE_ROWS;
    if (repeat & KEY_LEFT) record_change(&r, app.rec_row, -1);
    if (repeat & KEY_RIGHT) record_change(&r, app.rec_row, +1);
    if (repeat & KEY_L) for (int i = 0; i < 10; i++) record_change(&r, app.rec_row, -1);
    if (repeat & KEY_R) for (int i = 0; i < 10; i++) record_change(&r, app.rec_row, +1);
    int open_pick = -1;
    if (down & (KEY_Y | KEY_X)) {
        if (app.rec_row >= RE_UNIT1) open_pick = app.rec_row;
        else record_type(&r, (down & KEY_X) && app.rec_row == RE_TIME ? -RE_TIME : app.rec_row);
    }
    if (down & KEY_TOUCH) {
        for (int i = 0; i < RE_ROWS; i++) {
            float y = 6 + i * 32.0f;
            ui_button label = {0, y, 146, 30, NULL}, minus = {150, y, 76, 30, NULL}, plus = {230, y, 86, 30, NULL};
            if (i == RE_TIME) {  // [min] : [sec] boxes, typed separately
                ui_button mb = {150, y, 76, 30, NULL}, sb = {240, y, 76, 30, NULL};
                if (ui_button_hit(&mb, touch)) { app.rec_row = i; record_type(&r, RE_TIME); }
                if (ui_button_hit(&sb, touch)) { app.rec_row = i; record_type(&r, -RE_TIME); }
                if (ui_button_hit(&label, touch)) app.rec_row = i;
                continue;
            }
            if (ui_button_hit(&label, touch)) {
                app.rec_row = i;
                if (i >= RE_UNIT1) open_pick = i;
                else record_type(&r, i);
            }
            if (ui_button_hit(&minus, touch)) { app.rec_row = i; record_change(&r, i, -1); }
            if (ui_button_hit(&plus, touch)) { app.rec_row = i; record_change(&r, i, +1); }
        }
        if (ui_button_hit(&BTN_RC_BACK, touch)) down |= KEY_B;
    }
    if (memcmp(&r, &before, sizeof(r)) != 0) {
        fe_record_set(s, k, &r);
        app.dirty = true;
        LOGI("record %d (%s) edited: %d turns, %lu frames, units %d/%d classes %d/%d", k, fe_chapter_name(r.chapter),
             r.turns, (unsigned long)r.frames, r.unit[0], r.unit[1], r.cls[0], r.cls[1]);
    }
    if (open_pick >= 0) {
        int cur = open_pick == RE_UNIT1 ? r.unit[0] : open_pick == RE_UNIT2 ? r.unit[1]
                : open_pick == RE_CLASS1 ? r.cls[0] : r.cls[1];
        open_record_pick(open_pick, cur);
        return;
    }
    if (down & KEY_A) {
        if (app.rec_row >= RE_UNIT1) {
            int cur = app.rec_row == RE_UNIT1 ? r.unit[0] : app.rec_row == RE_UNIT2 ? r.unit[1]
                    : app.rec_row == RE_CLASS1 ? r.cls[0] : r.cls[1];
            open_record_pick(app.rec_row, cur);
            return;
        }
        fe_record t2 = r;
        record_type(&t2, app.rec_row);
        if (memcmp(&t2, &r, sizeof(r)) != 0) { fe_record_set(s, k, &t2); app.dirty = true; }
        return;
    }
    if (down & KEY_B) { app.st = ST_RECORDS; return; }

    char vals[RE_ROWS][40], t[24];
    fmt_time(t, sizeof(t), r.frames, false);
    snprintf(vals[RE_TURNS], 40, "%d", r.turns);
    snprintf(vals[RE_TIME], 40, "%s", t);
    for (int i = 0; i < 2; i++) {
        snprintf(vals[RE_UNIT1 + 2 * i], 40, "%s", r.unit[i] < 0 ? "-" : char_name(r.unit[i]));
        snprintf(vals[RE_CLASS1 + 2 * i], 40, "%s", r.cls[i] < 0 ? "-" : fe_class_name(r.cls[i]));
    }
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Record: %s%s", fe_chapter_name(r.chapter), app.dirty ? "  * unsaved" : "");
    for (int i = 0; i < RE_ROWS; i++) {
        float y = 34 + i * 24;
        if (i == app.rec_row) ui_rect(4, y - 2, TOP_W - 8, 22, CLR_SEL);
        ui_text(12, y, 0.55f, CLR_DIM, "%s", RE_LABELS[i]);
        ui_text(120, y, 0.55f, CLR_TEXT, "%s", vals[i]);
    }
    ui_text(8, 206, 0.42f, CLR_DIM, "A or tap: type / pick from a list   Left/Right: change   L/R: 10");
    ui_text(8, 222, 0.42f, CLR_DIM, "Time: tap minutes or seconds (Y: minutes, X: seconds)   B: back");

    ui_bottom();
    for (int i = 0; i < RE_ROWS; i++) {
        float y = 6 + i * 32.0f;
        if (i == app.rec_row) ui_rect(0, y - 1, BOTTOM_W, 32, CLR_SEL);
        ui_text(8, y + 2, 0.42f, CLR_DIM, "%s%s", RE_LABELS[i], i >= RE_UNIT1 ? "  (tap to pick)" : i == RE_TURNS ? "  (tap to type)" : "");
        ui_text(8, y + 14, 0.46f, CLR_TEXT, "%.18s", vals[i]);
        if (i == RE_TIME) {
            uint32_t sec = r.frames / 60;
            char mm[12], ss[12];
            snprintf(mm, sizeof(mm), "%lu min", (unsigned long)(sec / 60));
            snprintf(ss, sizeof(ss), "%02lu sec", (unsigned long)(sec % 60));
            ui_button mb = {150, y, 76, 30, mm}, sb = {240, y, 76, 30, ss};
            ui_button_draw(&mb, true);
            ui_text(230, y + 6, 0.5f, CLR_TEXT, ":");
            ui_button_draw(&sb, true);
            continue;
        }
        ui_button minus = {150, y, 76, 30, "<"}, plus = {230, y, 86, 30, ">"};
        ui_button_draw(&minus, true);
        ui_button_draw(&plus, true);
    }
    ui_button_draw(&BTN_RC_BACK, true);
    ui_end();
}

//---------------------------------------------------------------------------
// Barracks events
//---------------------------------------------------------------------------
static const ui_button BTN_BV_CLEAR = {4, 202, 110, 34, "Clear all"};

static void screen_barracks_open(void)
{
    if (!fe_barracks_ok(app.save)) {
        show_message("No Barracks data", CLR_WARN, ST_ARMY, "This save's Barracks event data was not recognized.");
        return;
    }
    app.bk_ev_sel = 0;
    app.bk_type_pick = false;
    app.toast_frames = 0;
    app.st = ST_BARRACKS;
}

static void barracks_pick_unit(int slot)
{
    fe_barracks_event ev;
    fe_barracks_get(app.save, app.bk_ev_sel, &ev);
    open_record_pick(slot ? RE_UNIT2 : RE_UNIT1, ev.unit[slot]);
    app.rp_target = 1;
}

static void barracks_set_type(int k, int type)
{
    fe_barracks_event ev;
    fe_barracks_get(app.save, k, &ev);
    ev.type = type;
    if (type != FE_BEV_TALK) ev.unit[1] = -1;  // only conversations have a second unit
    if (ev.type != FE_BEV_NONE && ev.unit[0] < 0) {  // a sensible default unit: the first in the army
        const fe_unit *u = app.vis_count ? &app.save->units[app.visible[0]] : NULL;
        if (u) ev.unit[0] = fe_unit_char_id(app.save, u);
    }
    fe_barracks_set(app.save, k, &ev);
    app.dirty = true;
    LOGI("barracks event %d: %s", k + 1, fe_barracks_names[ev.type]);
}

static void barracks_open_types(void)
{
    fe_barracks_event ev;
    fe_barracks_get(app.save, app.bk_ev_sel, &ev);
    app.bk_type_sel = ev.type;
    app.bk_type_pick = true;
}

// Event type list for the selected slot
static void screen_barracks_types(u32 down, u32 repeat, const touchPosition *touch)
{
    if (repeat & KEY_UP) app.bk_type_sel = (app.bk_type_sel + FE_BEV_TYPES - 1) % FE_BEV_TYPES;
    if (repeat & KEY_DOWN) app.bk_type_sel = (app.bk_type_sel + 1) % FE_BEV_TYPES;
    int pick = -1;
    if (down & (KEY_A | KEY_R)) pick = app.bk_type_sel;
    if (down & KEY_TOUCH) {
        for (int t = 0; t < FE_BEV_TYPES; t++) {
            ui_button b = {10, 4 + t * 28.0f, 300, 26, NULL};
            if (ui_button_hit(&b, touch)) pick = t;
        }
        if (ui_button_hit(&BTN_AR_DONE, touch)) down |= KEY_B;
    }
    if (pick >= 0) { barracks_set_type(app.bk_ev_sel, pick); app.bk_type_pick = false; return; }
    if (down & KEY_B) { app.bk_type_pick = false; return; }
    static const char *const info[FE_BEV_TYPES] = {
        "No event in this slot.", "A unit gains a stat boost.", "A unit gains experience.",
        "A unit gains weapon experience.", "A unit finds an item.", "Two units have a conversation.",
        "A unit's birthday."};
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Barracks event %d: type", app.bk_ev_sel + 1);
    ui_text(12, 40, 0.65f, CLR_ACCENT, "%s", fe_barracks_names[app.bk_type_sel]);
    ui_text_wrap(12, 70, 0.48f, CLR_TEXT, TOP_W - 24, info[app.bk_type_sel]);
    ui_text(8, 222, 0.42f, CLR_DIM, "Tap a type or A: pick   B: cancel");
    ui_bottom();
    for (int t = 0; t < FE_BEV_TYPES; t++) {
        ui_button b = {10, 4 + t * 28.0f, 300, 26, fe_barracks_names[t]};
        if (t == app.bk_type_sel) ui_rect(b.x - 2, b.y - 1, b.w + 4, b.h + 2, CLR_SEL);
        ui_button_draw(&b, true);
    }
    ui_button c = BTN_AR_DONE;
    c.label = "Cancel";
    ui_button_draw(&c, true);
    ui_end();
}

static void screen_barracks(u32 down, u32 repeat, const touchPosition *touch)
{
    fe_save *s = app.save;
    if (app.bk_type_pick) { screen_barracks_types(down, repeat, touch); return; }
    if (repeat & KEY_UP) app.bk_ev_sel = (app.bk_ev_sel + FE_BARRACKS_SLOTS - 1) % FE_BARRACKS_SLOTS;
    if (repeat & KEY_DOWN) app.bk_ev_sel = (app.bk_ev_sel + 1) % FE_BARRACKS_SLOTS;
    if (down & KEY_R) { barracks_open_types(); return; }
    fe_barracks_event cur_ev;
    fe_barracks_get(s, app.bk_ev_sel, &cur_ev);
    if (down & KEY_A) { barracks_pick_unit(0); return; }
    if ((down & KEY_X) && cur_ev.type != FE_BEV_TALK) down &= ~(u32)KEY_X;
    if (down & KEY_X) { barracks_pick_unit(1); return; }
    if (down & KEY_TOUCH) {
        for (int e = 0; e < FE_BARRACKS_SLOTS; e++) {
            float y = 6 + e * 38.0f;
            ui_button t = {4, y, 104, 34, NULL}, u1 = {112, y, 102, 34, NULL}, u2 = {218, y, 98, 34, NULL};
            if (ui_button_hit(&t, touch)) { app.bk_ev_sel = e; barracks_open_types(); return; }
            if (ui_button_hit(&u1, touch)) { app.bk_ev_sel = e; barracks_pick_unit(0); return; }
            if (ui_button_hit(&u2, touch)) {
                fe_barracks_event te;
                fe_barracks_get(s, e, &te);
                app.bk_ev_sel = e;
                if (te.type == FE_BEV_TALK) { barracks_pick_unit(1); return; }
            }
        }
        if (ui_button_hit(&BTN_BV_CLEAR, touch)) down |= KEY_Y;
        if (ui_button_hit(&BTN_AR_DONE, touch)) down |= KEY_B;
    }
    if (down & KEY_Y) {
        fe_barracks_event none = {FE_BEV_NONE, {-1, -1}, 0xFF};
        for (int e = 0; e < FE_BARRACKS_SLOTS; e++) fe_barracks_set(s, e, &none);
        app.dirty = true;
        LOGI("barracks events cleared");
        toast("All Barracks events cleared");
    }
    if (down & KEY_B) { app.st = ST_ARMY; return; }
    if (app.toast_frames > 0) app.toast_frames--;

    fe_barracks_event sel;
    fe_barracks_get(s, app.bk_ev_sel, &sel);
    ui_begin();
    ui_top();
    ui_rect(0, 0, TOP_W, 22, CLR_PANEL);
    ui_text(8, 3, 0.55f, CLR_ACCENT, "Barracks events%s", app.dirty ? "  * unsaved" : "");
    for (int e = 0; e < FE_BARRACKS_SLOTS; e++) {
        fe_barracks_event ev;
        fe_barracks_get(s, e, &ev);
        float y = 30 + e * 22;
        if (e == app.bk_ev_sel) ui_rect(8, y - 2, TOP_W - 16, 21, CLR_SEL);
        ui_text(16, y, 0.5f, ev.type ? CLR_TEXT : CLR_DIM, "%d. %s", e + 1, fe_barracks_names[ev.type]);
        if (ev.type)
            ui_text(170, y, 0.48f, CLR_ACCENT, "%s%s%s", ev.unit[0] < 0 ? "-" : char_name(ev.unit[0]),
                    ev.unit[1] >= 0 ? " & " : "", ev.unit[1] >= 0 ? char_name(ev.unit[1]) : "");
    }
    ui_text_wrap(12, 144, 0.42f, CLR_DIM, TOP_W - 24,
                 "Events wait in the Barracks for the next visit. Conversations use two units. "
                 "Experimental: the game normally picks these itself, so test on a copy first.");
    if (app.toast_frames > 0) ui_text(12, 190, 0.46f, CLR_GOOD, "%s", app.toast);
    ui_text(8, 206, 0.42f, CLR_DIM, "R or tap type: pick event   A: unit 1   X: unit 2 (conversations)");
    ui_text(8, 222, 0.42f, CLR_DIM, "Y: clear all   B: back");

    ui_bottom();
    for (int e = 0; e < FE_BARRACKS_SLOTS; e++) {
        fe_barracks_event ev;
        fe_barracks_get(s, e, &ev);
        float y = 6 + e * 38.0f;
        if (e == app.bk_ev_sel) ui_rect(0, y - 2, BOTTOM_W, 38, CLR_SEL);
        char a[24], b[24];
        snprintf(a, sizeof(a), "%.12s", ev.unit[0] < 0 ? "Unit 1" : char_name(ev.unit[0]));
        snprintf(b, sizeof(b), "%.12s", ev.type != FE_BEV_TALK ? "-" : ev.unit[1] < 0 ? "Unit 2" : char_name(ev.unit[1]));
        ui_button t = {4, y, 104, 34, fe_barracks_names[ev.type]}, u1 = {112, y, 102, 34, a}, u2 = {218, y, 98, 34, b};
        ui_button_draw(&t, true);
        ui_button_draw(&u1, ev.type != FE_BEV_NONE);
        ui_button_draw(&u2, ev.type == FE_BEV_TALK);
    }
    ui_button_draw(&BTN_BV_CLEAR, true);
    ui_button_draw(&BTN_AR_DONE, true);
    ui_end();
}

static void screen_confirm(u32 down, const touchPosition *touch, bool is_save)
{
    if (down & KEY_TOUCH) {
        if (ui_button_hit(&BTN_YES, touch)) down |= KEY_A;
        if (ui_button_hit(&BTN_NO, touch)) down |= KEY_B;
        if (is_save && ui_button_hit(&BTN_CF_BACKUP, touch)) down |= KEY_X;
    }
    if (is_save && (down & KEY_X)) {
        app.auto_backup = !app.auto_backup;
        settings_save();
    }
    if (down & KEY_A) {
        if (is_save) {
            do_save();
        } else {
            LOGI("discarded unsaved changes to slot %d", app.slot + 1);
            app.dirty = false;
            global_discard();
            app.st = ST_PICK_SLOT;
        }
        return;
    }
    if (down & KEY_B) { app.st = ST_UNITS; return; }

    ui_begin();
    ui_top();
    if (is_save) {
        ui_text(16, 16, 0.75f, CLR_ACCENT, "Write changes to Slot %d?", app.slot + 1);
        ui_text_wrap(16, 56, 0.55f, CLR_TEXT, TOP_W - 32,
                     app.auto_backup ? "First, the whole save is copied to sdmc:/3ds/FEAEditor/backups/.\n"
                                       "Then the edited slot is rebuilt and checked, written, committed and read back.\n\n"
                                       "If any step fails, nothing is written."
                                     : "Backups before saving are OFF (Backups screen), so no copy is made.\n"
                                       "The edited slot is rebuilt and checked, written, committed and read back.\n\n"
                                       "If any step fails, nothing is written.");
    } else {
        ui_text(16, 16, 0.75f, CLR_WARN, "Discard unsaved changes?");
        ui_text_wrap(16, 56, 0.55f, CLR_TEXT, TOP_W - 32, "Your edits to this slot will be lost.");
    }
    ui_bottom();
    if (is_save) {
        ui_button bb = BTN_CF_BACKUP;
        bb.label = app.auto_backup ? "Backup first: ON (X)" : "Backup first: OFF (X)";
        ui_rect(bb.x - 2, bb.y - 2, bb.w + 4, bb.h + 4, app.auto_backup ? CLR_GOOD : CLR_WARN);
        ui_button_draw(&bb, true);
    }
    ui_button_draw(&BTN_YES, true);
    ui_button_draw(&BTN_NO, true);
    ui_end();
}

static void screen_message(u32 down, const touchPosition *touch, bool is_fatal)
{
    if (!is_fatal && (down & KEY_TOUCH) && ui_button_hit(&BTN_OK, touch))
        down |= KEY_A;
    if (is_fatal && (down & (KEY_START | KEY_A | KEY_B))) { app.quit = true; return; }
    if (!is_fatal && (down & (KEY_A | KEY_B))) { app.st = app.msg_next; return; }

    ui_begin();
    ui_top();
    ui_text(16, 14, 0.75f, is_fatal ? CLR_ERR : app.msg_color, "%s", app.msg_title);
    ui_text_wrap(16, 52, 0.52f, CLR_TEXT, TOP_W - 32, app.msg);
    ui_bottom();
    ui_text_wrap(10, 10, 0.48f, CLR_DIM, BOTTOM_W - 20, "Details are in sdmc:/3ds/FEAEditor/debug.log");
    if (is_fatal)
        ui_text(10, 60, 0.55f, CLR_TEXT, "Press START to exit.");
    else
        ui_button_draw(&BTN_OK, true);
    ui_end();
}

//---------------------------------------------------------------------------
int main(void)
{
    gfxInitDefault();
    ui_init();
    hidSetRepeatParameters(20, 5);

    app.log_ok = log_init();
    settings_load();
    LOGI("Awakening Save Editor v%s starting", APP_VERSION);

    Result rc = amInit();
    app.am_ok = R_SUCCEEDED(rc);
    if (R_FAILED(LOG_RC("amInit", rc))) {
        fatal("Could not start the title manager service (rc=0x%08lX).\n"
              "Launch this from the Homebrew Launcher on Luma3DS.", (unsigned long)rc);
    } else {
        ui_busy("Awakening Save Editor", "Looking for Fire Emblem Awakening...");
        app.title_count = fs_find_titles(app.titles, FS_MAX_TITLES);
        if (app.title_count == 0)
            fatal("Fire Emblem Awakening was not found on the SD card or cartridge.\n\n"
                  "Insert the cartridge (or install the game) and start it once so it has save data.");
        else if (app.title_count == 1)
            open_title(0);
        else
            app.st = ST_PICK_TITLE;
    }

    while (!app.quit && aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 repeat = hidKeysDownRepeat();
        touchPosition touch;
        hidTouchRead(&touch);

        switch (app.st) {
        case ST_PICK_TITLE:      screen_pick_title(down, &touch); break;
        case ST_PICK_SLOT:       screen_pick_slot(down, &touch); break;
        case ST_UNITS:           screen_units(down, repeat, &touch); break;
        case ST_EDIT:            screen_edit(down, repeat, &touch); break;
        case ST_SKILLS:          screen_skills(down, repeat, &touch); break;
        case ST_CLASS:           screen_class(down, repeat, &touch); break;
        case ST_SUPPORTS:        screen_supports(down, repeat, &touch); break;
        case ST_ARMY:            screen_army(down, repeat, &touch); break;
        case ST_ITEMS:           screen_items(down, repeat, &touch); break;
        case ST_ITEM_PICK:       screen_item_pick(down, repeat, &touch); break;
        case ST_CONVOY:          screen_convoy(down, repeat, &touch); break;
        case ST_EXTRAS:          screen_extras(down, repeat, &touch); break;
        case ST_AVATAR:          screen_avatar(down, repeat, &touch); break;
        case ST_LOOK_PICK:       screen_look_pick(down, repeat, &touch); break;
        case ST_GLOBAL:          screen_global(down, repeat, &touch); break;
        case ST_GLOBAL_RESTORE:  screen_global_restore(down, repeat, &touch); break;
        case ST_ADD_UNIT:        screen_add_unit(down, repeat, &touch); break;
        case ST_MORE:            screen_more(down, repeat, &touch); break;
        case ST_ARMY_TOOLS:      screen_army_tools(down, repeat, &touch); break;
        case ST_WORLD_MAP:       screen_world_map(down, repeat, &touch); break;
        case ST_IMPORT_MENU:     screen_import_menu(down, repeat, &touch); break;
        case ST_IMPORT_PICK:     screen_import_pick(down, repeat, &touch); break;
        case ST_PARENTS:         screen_parents(down, repeat, &touch); break;
        case ST_BACKUPS:         screen_backups(down, repeat, &touch); break;
        case ST_FORGE:           screen_forge(down, repeat, &touch); break;
        case ST_RECORDS:         screen_records(down, repeat, &touch); break;
        case ST_RECORD_EDIT:     screen_record_edit(down, repeat, &touch); break;
        case ST_RECORD_PICK:     screen_record_pick(down, repeat, &touch); break;
        case ST_BARRACKS:        screen_barracks(down, repeat, &touch); break;
        case ST_CONFIRM_SAVE:    screen_confirm(down, &touch, true); break;
        case ST_CONFIRM_DISCARD: screen_confirm(down, &touch, false); break;
        case ST_MESSAGE:         screen_message(down, &touch, false); break;
        case ST_FATAL:           screen_message(down, &touch, true); break;
        }
    }

    if (app.dirty)
        LOGW("exiting with unsaved changes (discarded)");
    close_archive();
    if (app.save) {
        fe_save_free(app.save);
        free(app.save);
    }
    if (app.am_ok)
        amExit();
    log_close();
    ui_exit();
    gfxExit();
    return 0;
}
