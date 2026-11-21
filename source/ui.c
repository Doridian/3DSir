// Drawing helpers, see ui.h. Ported from Prox3DS.

#include "ui.h"

#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

u32 ui_col[COL_N];

static C3D_RenderTarget *top, *bot;
static char hints[256];
static C2D_TextBuf frame_buf, mono_buf;
static C2D_Text mono[95]; // ' ' .. '~'
static u8 ascii_w[128]; // glyph advances: wrapping measures a lot
static float font_scale = 1; // citro2d draws the system font this much larger than its advances

void ui_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    fontEnsureMapped();
    for(int c = 0; c < 128; c++) ascii_w[c] = fontGetCharWidthInfo(NULL, fontGlyphIndexFromCodePoint(NULL, c))->charWidth;
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    frame_buf = C2D_TextBufNew(8192);
    mono_buf = C2D_TextBufNew(256);
    for(int c = 0; c < 95; c++) {
        char s[2] = {32 + c, 0};
        C2D_TextParse(&mono[c], mono_buf, s);
        C2D_TextOptimize(&mono[c]);
    }
    float w, h;
    C2D_TextGetDimensions(&mono['M' - 32], 1, 1, &w, &h);
    if(mono['M' - 32].width > 0) font_scale = w / mono['M' - 32].width;
    // The icon's colors: IR red on a dark night, amber for the cursor.
    static const u32 rgb[COL_N] = {
        [COL_BG] = 0x0E0709,     [COL_PANEL] = 0x1C0E13, [COL_BAR] = 0x2B141C, [COL_TEXT] = 0xF4EEF2,
        [COL_DIM] = 0xA3828E,    [COL_ACCENT] = 0xFF2E63, [COL_SEL] = 0x5A1830, [COL_CURSOR] = 0xFFA33D,
        [COL_WARN] = 0xFFD43D,   [COL_ERR] = 0xFF5A4D,   [COL_OK] = 0x3DFFA0,
    };
    for(int i = 0; i < COL_N; i++) ui_col[i] = C2D_Color32(rgb[i] >> 16, rgb[i] >> 8 & 0xFF, rgb[i] & 0xFF, 0xFF);
}

void ui_exit(void) {
    C2D_TextBufDelete(mono_buf);
    C2D_TextBufDelete(frame_buf);
    C2D_Fini();
    C3D_Fini();
}

void ui_frame_begin(void) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top, ui_col[COL_BG]);
    C2D_TargetClear(bot, ui_col[COL_BG]);
    C2D_SceneBegin(top);
    C2D_TextBufClear(frame_buf);
    *hints = 0;
}

void ui_bottom(void) {
    C2D_SceneBegin(bot);
}

void ui_frame_end(void) {
    C3D_FrameEnd(0);
}

static bool touch_down, touch_rep;
static float touch_x, touch_y;

void ui_touch(bool down, bool repeat, float x, float y) {
    touch_down = down, touch_rep = repeat, touch_x = x, touch_y = y;
}

// A button's face: an accent edge under it, a corner tick
static void face(float x, float y, float w, float h, u32 color, bool hit) {
    ui_rect(x, y, w, h, hit ? ui_col[COL_ACCENT] : color);
    ui_rect(x, y + h - 1, w, 1, hit ? ui_col[COL_CURSOR] : ui_col[COL_ACCENT]);
    ui_rect(x, y, 3, 1, ui_col[COL_DIM]);
    ui_rect(x, y, 1, 3, ui_col[COL_DIM]);
}

static bool button(float x, float y, float w, float h, u32 color, const char* label, bool rep) {
    bool hit = touch_down && (rep || !touch_rep) && touch_x >= x && touch_x < x + w && touch_y >= y && touch_y < y + h;
    face(x, y, w, h, color, hit);
    char t[64];
    snprintf(t, sizeof(t), "%s", label);
    ui_text_center(x + w / 2, y + (h - 16) / 2, S_BODY, ui_col[COL_TEXT], "%s", ui_fit(t, w - 6, S_BODY));
    if(hit) touch_down = false; // one button per tap
    return hit;
}

bool ui_button(float x, float y, float w, float h, u32 color, const char* label) {
    return button(x, y, w, h, color, label, false);
}

bool ui_button_rep(float x, float y, float w, float h, u32 color, const char* label) {
    return button(x, y, w, h, color, label, true);
}

void ui_rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
}

static float draw_v(float x, float y, float scale, u32 color, u32 flags, const char* fmt, va_list va) {
    char s[512];
    vsnprintf(s, sizeof(s), fmt, va);
    C2D_Text t;
    C2D_TextParse(&t, frame_buf, s);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
    return t.width * scale * font_scale;
}

float ui_text(float x, float y, float scale, u32 color, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    float w = draw_v(x, y, scale, color, 0, fmt, va);
    va_end(va);
    return w;
}

void ui_text_right(float right, float y, float scale, u32 color, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    draw_v(right, y, scale, color, C2D_AlignRight, fmt, va);
    va_end(va);
}

void ui_text_center(float cx, float y, float scale, u32 color, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    draw_v(cx, y, scale, color, C2D_AlignCenter, fmt, va);
    va_end(va);
}

// Width of s[0..n) (one line): the glyphs' advances, as C2D_TextParse measures them.
static float width_n(const char* s, int n, float scale) {
    float w = 0;
    const u8 *p = (const u8*)s, *e = p + n;
    while(p < e && *p) {
        if(*p < 128) {
            w += ascii_w[*p++];
            continue;
        }
        u32 cp;
        ssize_t k = decode_utf8(&cp, p);
        if(k <= 0) break;
        p += k;
        w += fontGetCharWidthInfo(NULL, fontGlyphIndexFromCodePoint(NULL, cp))->charWidth;
    }
    return w * scale * font_scale;
}

float ui_text_width(float scale, const char* s) {
    float w = 0;
    for(;;) {
        const char* e = strchr(s, '\n');
        int n = e ? e - s : (int)strlen(s);
        float lw = width_n(s, n, scale);
        if(lw > w) w = lw;
        if(!e) return w;
        s = e + 1;
    }
}

char* ui_fit(char* s, float w, float scale) {
    int n = strlen(s);
    if(width_n(s, n, scale) <= w) return s;
    float dots = width_n("...", 3, scale);
    while(n > 0 && (width_n(s, n, scale) + dots > w || n + 3 > (int)strlen(s))) {
        do n--;
        while(n > 0 && (s[n] & 0xC0) == 0x80); // whole UTF-8 sequences
    }
    strcpy(s + n, "...");
    return s;
}

static float line_h(float scale) {
    return roundf(scale / S_SMALL * SMALL_H);
}

// Lines of at most w: broken at spaces, inside a word only when it doesn't fit a line alone, and at '\n'.
static int wrap(float x, float y, float w, float scale, u32 color, bool draw, const char* text) {
    int lines = 0;
    for(const char* p = text;;) {
        const char* end = strchr(p, '\n');
        if(!end) end = p + strlen(p);
        do {
            const char *e = p, *q = p;
            while(q < end) { // add the next word (with the spaces before it) while it fits
                const char* nx = q;
                while(nx < end && *nx == ' ') nx++;
                while(nx < end && *nx != ' ') nx++;
                if(width_n(p, nx - p, scale) > w) break;
                e = q = nx;
            }
            if(e == p) { // a word wider than the line: as many characters as fit, at least one
                e = p + 1;
                while(e < end && (*e & 0xC0) == 0x80) e++;
                for(const char* c = e; c < end && *c != ' ';) {
                    do c++;
                    while(c < end && (*c & 0xC0) == 0x80);
                    if(width_n(p, c - p, scale) > w) break;
                    e = c;
                }
            }
            if(draw) ui_text(x, y + lines * line_h(scale), scale, color, "%.*s", (int)(e - p), p);
            lines++;
            p = e;
            while(p < end && *p == ' ') p++;
        } while(p < end);
        if(!*end) return lines;
        p = end + 1;
    }
}

float ui_text_wrap(float x, float y, float w, float scale, u32 color, const char* text) {
    return y + wrap(x, y, w, scale, color, true, text) * line_h(scale);
}

int ui_wrap_lines(float w, float scale, const char* text) {
    return wrap(0, 0, w, scale, 0, false, text);
}

void ui_mono_s(float x, float y, float scale, float cell, u32 color, const char* s) {
    for(; *s; s++, x += cell) {
        int c = (*s >= 32 && *s < 127) ? *s - 32 : '.' - 32;
        if(!c) continue;
        C2D_DrawText(&mono[c], C2D_WithColor, x + (cell - mono[c].width * scale * font_scale) / 2, y, 0.5f, scale, scale, color);
    }
}

void ui_mono(float x, float y, u32 color, const char* s) {
    ui_mono_s(x, y, S_MONO, MONO_W, color, s);
}

void ui_header(const char* title, const char* right) {
    char t[256];
    snprintf(t, sizeof(t), "%s", title);
    ui_rect(0, 0, UI_W, 20, ui_col[COL_BAR]);
    ui_rect(0, 19, UI_W, 1, ui_col[COL_ACCENT]);
    ui_text(6, 2, S_BODY, ui_col[COL_TEXT], "%s", ui_fit(t, UI_W - 12 - (right ? ui_text_width(S_SMALL, right) + 12 : 0), S_BODY));
    if(right) ui_text_right(UI_W - 6, 3, S_SMALL, ui_col[COL_DIM], "%s", right);
}

void ui_footer(const char* h) {
    ui_rect(0, UI_H - 18, UI_W, 18, ui_col[COL_BAR]);
    ui_rect(0, UI_H - 18, UI_W, 1, ui_col[COL_SEL]);
    ui_text(6, UI_H - 16, S_SMALL, ui_col[COL_DIM], "%s", h);
    ui_hints(h);
}

void ui_sel(float x, float y, float w, float h) {
    ui_rect(x, y, w, h, ui_col[COL_SEL]);
    ui_rect(x, y, 2, h, ui_col[COL_CURSOR]);
}

void ui_hints(const char* h) {
    snprintf(hints, sizeof(hints), "%s", h);
}

const char* ui_hints_now(void) {
    return hints;
}

float ui_box(float w, float h, const char* title) {
    float x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    ui_rect(0, 0, UI_W, UI_H, C2D_Color32(0x0E, 0x07, 0x09, 0xA8));
    ui_rect(x - 1, y - 1, w + 2, h + 2, ui_col[COL_ACCENT]);
    ui_rect(x, y, w, h, ui_col[COL_PANEL]);
    ui_rect(x, y, w, 18, ui_col[COL_BAR]);
    ui_rect(x, y + 18, w, 1, ui_col[COL_SEL]);
    // brackets on the corners
    u32 c = ui_col[COL_CURSOR];
    float r = x + w, b = y + h, a = 9;
    ui_rect(x - 3, y - 3, a, 2, c), ui_rect(x - 3, y - 3, 2, a, c);
    ui_rect(r + 3 - a, y - 3, a, 2, c), ui_rect(r + 1, y - 3, 2, a, c);
    ui_rect(x - 3, b + 1, a, 2, c), ui_rect(x - 3, b + 3 - a, 2, a, c);
    ui_rect(r + 3 - a, b + 1, a, 2, c), ui_rect(r + 1, b + 3 - a, 2, a, c);
    char t[128];
    snprintf(t, sizeof(t), "%s", title);
    ui_text(x + 6, y + 1, S_BODY, ui_col[COL_TEXT], "%s", ui_fit(t, w - 12, S_BODY));
    return y + 22;
}

void ui_list_fix(UiList* l, int n, int visible) {
    if(n <= 0) {
        l->sel = l->top = 0;
        return;
    }
    if(l->sel >= n) l->sel = n - 1;
    if(l->sel < 0) l->sel = 0;
    if(l->sel < l->top) l->top = l->sel;
    if(l->sel >= l->top + visible) l->top = l->sel - visible + 1;
    if(l->top > (n > visible ? n - visible : 0)) l->top = n > visible ? n - visible : 0;
}

void ui_list_nav(UiList* l, int n, int visible, u32 repeat) {
    if(n > 0) {
        if(repeat & KEY_UP) l->sel = l->sel > 0 ? l->sel - 1 : n - 1;
        if(repeat & KEY_DOWN) l->sel = l->sel < n - 1 ? l->sel + 1 : 0;
        if(repeat & KEY_LEFT) l->sel = l->sel > visible ? l->sel - visible : 0;
        if(repeat & KEY_RIGHT) l->sel = l->sel + visible < n ? l->sel + visible : n - 1;
    }
    ui_list_fix(l, n, visible);
}
