#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define TOP_W    400
#define BOTTOM_W 320
#define SCREEN_H 240

// Palette
#define CLR_BG       C2D_Color32(0x1B, 0x1F, 0x27, 0xFF)
#define CLR_PANEL    C2D_Color32(0x26, 0x2C, 0x37, 0xFF)
#define CLR_SEL      C2D_Color32(0x3A, 0x5A, 0x8C, 0xFF)
#define CLR_TEXT     C2D_Color32(0xEE, 0xEE, 0xEE, 0xFF)
#define CLR_DIM      C2D_Color32(0x9A, 0xA3, 0xB2, 0xFF)
#define CLR_ACCENT   C2D_Color32(0xF2, 0xC1, 0x4E, 0xFF)
#define CLR_GOOD     C2D_Color32(0x7E, 0xD3, 0x7E, 0xFF)
#define CLR_WARN     C2D_Color32(0xFF, 0x9E, 0x5E, 0xFF)
#define CLR_ERR      C2D_Color32(0xFF, 0x6B, 0x6B, 0xFF)
#define CLR_BTN      C2D_Color32(0x34, 0x3D, 0x4C, 0xFF)

typedef struct {
    float x, y, w, h;
    const char *label;
} ui_button;

void ui_init(void);
void ui_exit(void);

// Frame structure: ui_begin(); ui_top(); ...draw...; ui_bottom(); ...draw...; ui_end();
void ui_begin(void);
void ui_top(void);
void ui_bottom(void);
void ui_end(void);

void ui_rect(float x, float y, float w, float h, u32 color);
// Draws printf-style text. Returns the text height.
float ui_text(float x, float y, float scale, u32 color, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void ui_text_right(float right_x, float y, float scale, u32 color, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void ui_text_wrap(float x, float y, float scale, u32 color, float width, const char *text);

void ui_button_draw(const ui_button *b, bool enabled);
bool ui_button_hit(const ui_button *b, const touchPosition *t);

// Draws a single "please wait" frame before a blocking operation.
void ui_busy(const char *title, const char *msg);
