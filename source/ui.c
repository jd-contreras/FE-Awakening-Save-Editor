#include "ui.h"

#include <string.h>

#include <stdarg.h>
#include <stdio.h>

static C3D_RenderTarget *s_top, *s_bottom;
static C2D_TextBuf s_buf;

void ui_init(void)
{
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    s_buf = C2D_TextBufNew(4096);
}

void ui_exit(void)
{
    C2D_TextBufDelete(s_buf);
    C2D_Fini();
    C3D_Fini();
}

void ui_begin(void)
{
    C2D_TextBufClear(s_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void ui_top(void)
{
    C2D_TargetClear(s_top, CLR_BG);
    C2D_SceneBegin(s_top);
}

void ui_bottom(void)
{
    C2D_TargetClear(s_bottom, CLR_BG);
    C2D_SceneBegin(s_bottom);
}

void ui_end(void)
{
    C3D_FrameEnd(0);
}

void ui_rect(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

static void draw_str(float x, float y, float scale, u32 color, u32 flags, float wrap, const char *s, float *h_out)
{
    C2D_Text t;
    C2D_TextParse(&t, s_buf, s);
    C2D_TextOptimize(&t);
    if (flags & C2D_WordWrap)
        C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color, wrap);
    else
        C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
    if (h_out) {
        float w;
        C2D_TextGetDimensions(&t, scale, scale, &w, h_out);
    }
}

float ui_text(float x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    float h = 0;
    draw_str(x, y, scale, color, 0, 0, buf, &h);
    return h;
}

void ui_text_right(float right_x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    draw_str(right_x, y, scale, color, C2D_AlignRight, 0, buf, NULL);
}

void ui_text_wrap(float x, float y, float scale, u32 color, float width, const char *text)
{
    draw_str(x, y, scale, color, C2D_WordWrap, width, text, NULL);
}

void ui_button_draw(const ui_button *b, bool enabled)
{
    ui_rect(b->x, b->y, b->w, b->h, enabled ? CLR_BTN : CLR_PANEL);
    if (!b->label) return;
    // ~9.5 px per character at scale 0.55: shrink labels that would overflow narrow buttons
    float scale = 0.55f, need = (float)strlen(b->label) * 9.5f;
    if (need > b->w - 6) scale = 0.55f * (b->w - 6) / need;
    if (scale < 0.38f) scale = 0.38f;
    draw_str(b->x + b->w / 2, b->y + b->h / 2 - 8 * scale / 0.55f, scale, enabled ? CLR_TEXT : CLR_DIM,
             C2D_AlignCenter, 0, b->label, NULL);
}

bool ui_button_hit(const ui_button *b, const touchPosition *t)
{
    return t->px >= b->x && t->px < b->x + b->w && t->py >= b->y && t->py < b->y + b->h;
}

void ui_busy(const char *title, const char *msg)
{
    ui_begin();
    ui_top();
    ui_text(16, 16, 0.75f, CLR_ACCENT, "%s", title);
    ui_text_wrap(16, 56, 0.55f, CLR_TEXT, TOP_W - 32, msg);
    ui_bottom();
    ui_text(16, 16, 0.55f, CLR_DIM, "Please wait. Do not power off.");
    ui_end();
}
