// Minimal libctru stand-in for a PC syntax/type check (tests/check_3ds_syntax.sh).
// Signatures mirror libctru; nothing here runs.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef s32 Result; typedef u32 Handle; typedef u64 FS_Archive;
#define R_FAILED(r) ((s32)(r) < 0)
#define R_SUCCEEDED(r) ((s32)(r) >= 0)
#define R_LEVEL(r) (((r)>>27)&0x1F)
#define R_SUMMARY(r) (((r)>>21)&0x3F)
#define R_MODULE(r) (((r)>>10)&0xFF)
#define R_DESCRIPTION(r) ((r)&0x3FF)
#define MAKERESULT(l,s,m,d) ((((l)&0x1F)<<27)|(((s)&0x3F)<<21)|(((m)&0xFF)<<10)|((d)&0x3FF))
enum { RL_FATAL=31, RL_PERMANENT=27 }; enum { RS_OUTOFRESOURCE=3, RS_INVALIDSTATE=5, RS_INVALIDARG=7 };
enum { RM_APPLICATION=254 }; enum { RD_OUT_OF_MEMORY=1011, RD_INVALID_SIZE=1012 };
typedef enum { MEDIATYPE_NAND=0, MEDIATYPE_SD=1, MEDIATYPE_GAME_CARD=2 } FS_MediaType;
typedef enum { PATH_INVALID=0, PATH_EMPTY=1, PATH_BINARY=2, PATH_ASCII=3, PATH_UTF16=4 } FS_PathType;
typedef struct { FS_PathType type; u32 size; const void *data; } FS_Path;
typedef enum { ARCHIVE_USER_SAVEDATA=4 } FS_ArchiveID;
typedef enum { ARCHIVE_ACTION_COMMIT_SAVE_DATA=0 } FS_ArchiveAction;
enum { FS_OPEN_READ=1, FS_OPEN_WRITE=2, FS_OPEN_CREATE=4 }; enum { FS_WRITE_FLUSH=1 }; enum { FS_ATTRIBUTE_DIRECTORY=1 };
typedef struct { u16 name[0x106]; char shortName[0x0A]; char shortExt[0x04]; u8 valid; u8 reserved; u32 attributes; u64 fileSize; } FS_DirectoryEntry;
Result FSUSER_OpenArchive(FS_Archive*, FS_ArchiveID, FS_Path);
Result FSUSER_CloseArchive(FS_Archive);
Result FSUSER_OpenDirectory(Handle*, FS_Archive, FS_Path);
Result FSDIR_Read(Handle, u32*, u32, FS_DirectoryEntry*); Result FSDIR_Close(Handle);
Result FSUSER_OpenFile(Handle*, FS_Archive, FS_Path, u32, u32);
Result FSFILE_GetSize(Handle, u64*); Result FSFILE_Read(Handle, u32*, u64, void*, u32);
Result FSFILE_Write(Handle, u32*, u64, const void*, u32, u32); Result FSFILE_Close(Handle);
Result FSUSER_DeleteFile(FS_Archive, FS_Path); Result FSUSER_CreateFile(FS_Archive, FS_Path, u32, u64);
Result FSUSER_ControlArchive(FS_Archive, FS_ArchiveAction, void*, u32, void*, u32);
Result amInit(void); void amExit(void);
Result AM_GetTitleCount(FS_MediaType, u32*); Result AM_GetTitleList(u32*, FS_MediaType, u32, u64*);
Result AM_GetTitleProductCode(FS_MediaType, u64, char*);
void gfxInitDefault(void); void gfxExit(void); bool aptMainLoop(void);
enum { KEY_A=1, KEY_B=2, KEY_SELECT=4, KEY_START=8, KEY_RIGHT=16, KEY_LEFT=32, KEY_UP=64, KEY_DOWN=128, KEY_R=256, KEY_L=512, KEY_X=1024, KEY_Y=2048, KEY_ZL=1<<14, KEY_ZR=1<<15, KEY_TOUCH=1<<20 };
static inline Result romfsMountFromTitle(u64 tid, FS_MediaType m, const char *name) { (void)tid; (void)m; (void)name; return 0; }
static inline Result romfsUnmount(const char *name) { (void)name; return 0; }
typedef struct { u16 px, py; } touchPosition;
void hidScanInput(void); u32 hidKeysDown(void); u32 hidKeysDownRepeat(void); void hidTouchRead(touchPosition*);
void hidSetRepeatParameters(u32, u32);
typedef enum { SWKBD_TYPE_NORMAL=0, SWKBD_TYPE_QWERTY, SWKBD_TYPE_NUMPAD, SWKBD_TYPE_WESTERN } SwkbdType;
typedef enum { SWKBD_ANYTHING=0, SWKBD_NOTEMPTY, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_NOTBLANK, SWKBD_FIXEDLEN } SwkbdValidInput;
typedef enum { SWKBD_BUTTON_LEFT=0, SWKBD_BUTTON_MIDDLE, SWKBD_BUTTON_RIGHT, SWKBD_BUTTON_CONFIRM=SWKBD_BUTTON_RIGHT, SWKBD_BUTTON_NONE } SwkbdButton;
typedef struct { int dummy; } SwkbdState;
void swkbdInit(SwkbdState*, SwkbdType, int, int); void swkbdSetHintText(SwkbdState*, const char*);
void swkbdSetInitialText(SwkbdState*, const char*); void swkbdSetValidation(SwkbdState*, SwkbdValidInput, u32, u32);
SwkbdButton swkbdInputText(SwkbdState*, char*, size_t);
