#pragma once

#include <citro2d.h>

// Drawing with citro2d and the system font, the same look as Prox3DS in IR reds. Everything works on the
// top screen with the buttons; the bottom screen doubles them for touch (the footer's hints as buttons, a
// D-pad, the log) or shows a screen's own keypad.

#define UI_W 400
#define UI_H 240
#define UI_BW 320 // the bottom screen, as tall

#define S_TITLE 0.55f
#define S_BODY 0.5f
#define S_SMALL 0.42f
#define S_MONO 0.40f
#define S_LOG 0.38f // 12 px lines
#define MONO_W 7.0f // fixed cell of ui_mono
#define LINE_H 15 // an S_BODY line
#define SMALL_H 13 // an S_SMALL line

// System font glyphs for the buttons
#define BTN_A ""
#define BTN_B ""
#define BTN_X ""
#define BTN_Y ""
#define BTN_L ""
#define BTN_R ""
#define BTN_DPAD ""
#define BTN_UD ""
#define BTN_LR ""

enum { COL_BG, COL_PANEL, COL_BAR, COL_TEXT, COL_DIM, COL_ACCENT, COL_SEL, COL_CURSOR, COL_WARN, COL_ERR, COL_OK, COL_N };
extern u32 ui_col[COL_N];

void ui_init(void);
void ui_exit(void);
void ui_frame_begin(void);
void ui_frame_end(void);
void ui_bottom(void); // draws from here to ui_frame_end go to the bottom screen

// Touch, immediate mode: the loop hands over each frame's touch (ui_touch), and a button drawn on the
// bottom screen that frame takes it or not. A touch held past the repeat delay comes again every few
// frames as a repeat, which only ui_button_rep takes.
void ui_touch(bool down, bool repeat, float x, float y);
bool ui_button(float x, float y, float w, float h, u32 color, const char* label); // true = tapped now
bool ui_button_rep(float x, float y, float w, float h, u32 color, const char* label);
// The footer's hints ("<button glyph> what it does" ...), for the bottom screen's touch buttons.
void ui_hints(const char* hints);
const char* ui_hints_now(void);

void ui_rect(float x, float y, float w, float h, u32 color);
float ui_text(float x, float y, float scale, u32 color, const char* fmt, ...) __attribute__((format(printf, 5, 6)));
void ui_text_right(float right, float y, float scale, u32 color, const char* fmt, ...) __attribute__((format(printf, 5, 6)));
void ui_text_center(float cx, float y, float scale, u32 color, const char* fmt, ...) __attribute__((format(printf, 5, 6)));
// Word-wrapped at w (and at '\n'); returns the y below it.
float ui_text_wrap(float x, float y, float w, float scale, u32 color, const char* text);
int ui_wrap_lines(float w, float scale, const char* text); // how many lines ui_text_wrap would draw
float ui_text_width(float scale, const char* s); // widest line, without drawing
char* ui_fit(char* s, float w, float scale); // cut to fit w with "...", in place; returns s
// Printable ASCII in fixed cells; anything else draws as '.'.
void ui_mono(float x, float y, u32 color, const char* s);
void ui_mono_s(float x, float y, float scale, float cell, u32 color, const char* s);
void ui_header(const char* title, const char* right);
void ui_footer(const char* hints);
void ui_sel(float x, float y, float w, float h); // a selected list row
float ui_box(float w, float h, const char* title); // a centered box with a title bar; -> content y

// Selectable list state: selection and first visible row.
typedef struct {
    int sel, top;
} UiList;
void ui_list_nav(UiList* l, int n, int visible, u32 repeat);
void ui_list_fix(UiList* l, int n, int visible); // keeps sel in range and visible
