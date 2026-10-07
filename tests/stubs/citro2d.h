#pragma once
#include <3ds.h>
typedef struct C3D_RenderTarget C3D_RenderTarget; typedef struct C2D_TextBuf_s *C2D_TextBuf;
typedef struct { C2D_TextBuf buf; size_t begin, end; float width; u32 lines, words; void *font; } C2D_Text;
typedef enum { GFX_TOP=0, GFX_BOTTOM=1 } gfxScreen_t; typedef enum { GFX_LEFT=0, GFX_RIGHT=1 } gfx3dSide_t;
#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C2D_DEFAULT_MAX_OBJECTS 4096
#define C3D_FRAME_SYNCDRAW 1
enum { C2D_AtBaseline=1, C2D_WithColor=2, C2D_AlignLeft=0, C2D_AlignRight=4, C2D_AlignCenter=8, C2D_AlignJustified=12, C2D_WordWrap=16 };
static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a) { return r | (g<<8) | (b<<16) | ((u32)a<<24); }
bool C3D_Init(size_t); void C3D_Fini(void); bool C2D_Init(size_t); void C2D_Fini(void); void C2D_Prepare(void);
C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t, gfx3dSide_t);
C2D_TextBuf C2D_TextBufNew(size_t); void C2D_TextBufDelete(C2D_TextBuf); void C2D_TextBufClear(C2D_TextBuf);
bool C3D_FrameBegin(u8); void C3D_FrameEnd(u8); void C2D_TargetClear(C3D_RenderTarget*, u32); void C2D_SceneBegin(C3D_RenderTarget*);
bool C2D_DrawRectSolid(float, float, float, float, float, u32);
const char *C2D_TextParse(C2D_Text*, C2D_TextBuf, const char*); void C2D_TextOptimize(const C2D_Text*);
void C2D_DrawText(const C2D_Text*, u32, float, float, float, float, float, ...);
void C2D_TextGetDimensions(const C2D_Text*, float, float, float*, float*);
