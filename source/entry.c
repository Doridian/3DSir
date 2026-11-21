// Text entry: an on-screen keyboard on the bottom screen, as in Prox3DS (the system keyboard applet can
// hang for good when closed during its opening animation). The top screen shows the text and its cursor.

#include <stdio.h>
#include <string.h>

#include "app.h"

#define TEXT_MAX 128
#define TEXT_COLS 52 // characters per line on the top screen
#define TEXT_ROWS 4

static struct {
    char title[64], what[256];
    char s[TEXT_MAX + 1];
    int n, cur, max;
    int shift; // 0 off, 1 the next letter, 2 caps lock
    bool sym;
    void (*done)(const char* s);
} t;

static void t_insert(char c) {
    if(t.n >= t.max) return;
    memmove(t.s + t.cur + 1, t.s + t.cur, t.n - t.cur + 1);
    t.s[t.cur++] = c;
    t.n++;
    if(t.shift == 1) t.shift = 0;
}

static void t_del(void) {
    if(!t.cur) return;
    memmove(t.s + t.cur - 1, t.s + t.cur, t.n - t.cur + 1);
    t.cur--, t.n--;
}

static void t_draw(void) {
    char right[40];
    snprintf(right, sizeof(right), "%d / %d", t.n, t.max);
    ui_header(t.title, right);
    float y = ui_text_wrap(12, 26, UI_W - 24, S_SMALL, ui_col[COL_TEXT], t.what) + 8;
    int cols = TEXT_COLS, lines = t.n / cols + 1, cl = t.cur / cols, top = cl >= TEXT_ROWS ? cl - TEXT_ROWS + 1 : 0;
    float w = cols * MONO_W, x0 = (UI_W - w) / 2;
    ui_rect(x0 - 4, y - 3, w + 8, TEXT_ROWS * 13 + 4, ui_col[COL_PANEL]);
    ui_rect(x0 - 4, y + TEXT_ROWS * 13, w + 8, 1, ui_col[COL_ACCENT]);
    for(int l = top; l < lines && l < top + TEXT_ROWS; l++, y += 13) {
        int from = l * cols, k = t.n - from < cols ? t.n - from : cols;
        for(int i = 0; i < k; i++) {
            char c[2] = {t.s[from + i], 0};
            ui_mono(x0 + i * MONO_W, y, ui_col[COL_TEXT], c);
        }
        if(cl == l) ui_rect(x0 + (t.cur % cols) * MONO_W - 1, y - 1, 1.5f, 12, ui_col[COL_CURSOR]);
    }
    ui_footer(BTN_LR " cursor   " BTN_Y " delete   " BTN_X " clear   " BTN_A " done   " BTN_B " cancel");
}

static void t_finish(void) {
    static char out[TEXT_MAX + 1];
    memcpy(out, t.s, t.n + 1);
    app_back();
    t.done(out);
}

static void t_input(u32 down, u32 repeat) {
    if((repeat & KEY_LEFT) && t.cur) t.cur--;
    if((repeat & KEY_RIGHT) && t.cur < t.n) t.cur++;
    if((repeat & KEY_UP) && t.cur >= TEXT_COLS) t.cur -= TEXT_COLS;
    if(repeat & KEY_DOWN) t.cur = t.cur + TEXT_COLS <= t.n ? t.cur + TEXT_COLS : t.n;
    if(repeat & KEY_Y) t_del();
    if(down & KEY_X) t.n = t.cur = 0, t.s[0] = 0;
    if(down & KEY_A) t_finish();
    else if(down & KEY_B) app_back();
}

// Four rows of ten; letters follow shift. The symbol layer keeps the digits.
static const char* const layers[2][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm_.-"},
    {"1234567890", "!@#$%^&*()", "=+[]{}~`|:", ";\",<>?/\\.-"},
};

static void t_bottom(void) {
    u32 key = ui_col[COL_BAR], side = ui_col[COL_PANEL];
    float w = 30, h = 36, g = 2, x0 = (UI_BW - 10 * w - 9 * g) / 2, y0 = 6;
    for(int r = 0; r < 4; r++) {
        const char* row = layers[t.sym][r];
        for(int c = 0; c < 10 && row[c]; c++) {
            char ch = row[c];
            if(!t.sym && t.shift && ch >= 'a' && ch <= 'z') ch -= 32;
            char l[2] = {ch, 0};
            if(ui_button_rep(x0 + c * (w + g), y0 + r * (h + g), w, h, key, l)) t_insert(ch);
        }
    }
    // shift / symbols / space / delete / done
    float y = y0 + 4 * (h + g), x = x0, bw = 10 * w + 9 * g;
    float ws[5] = {46, 46, 0, 46, 54};
    ws[2] = bw - ws[0] - ws[1] - ws[3] - ws[4] - 4 * g;
    const char* shift_l = t.shift == 2 ? "CAPS" : "Shift";
    if(ui_button(x, y, ws[0], h, t.shift ? ui_col[COL_SEL] : side, t.sym ? "" : shift_l) && !t.sym) t.shift = (t.shift + 1) % 3;
    x += ws[0] + g;
    if(ui_button(x, y, ws[1], h, t.sym ? ui_col[COL_SEL] : side, t.sym ? "abc" : "#+=")) t.sym = !t.sym;
    x += ws[1] + g;
    if(ui_button_rep(x, y, ws[2], h, key, "space")) t_insert(' ');
    x += ws[2] + g;
    if(ui_button_rep(x, y, ws[3], h, side, "Del")) t_del();
    x += ws[3] + g;
    if(ui_button(x, y, ws[4], h, ui_col[COL_SEL], "Done")) app_press(KEY_A);
    if(ui_button(x0, y + h + g + 4, bw / 2 - 1, 24, side, "Cancel")) app_press(KEY_B);
    if(ui_button(x0 + bw / 2 + 1, y + h + g + 4, bw / 2 - 1, 24, side, "Clear")) app_press(KEY_X);
}

static const Screen text_screen = {NULL, t_input, t_draw, t_bottom};

void entry_text(const char* title, const char* what, const char* init, u32 max, void (*done)(const char* s)) {
    snprintf(t.title, sizeof(t.title), "%s", title);
    snprintf(t.what, sizeof(t.what), "%s", what);
    t.max = max < 1 ? 1 : max > TEXT_MAX ? TEXT_MAX : max;
    t.n = 0;
    for(const char* c = init ? init : ""; *c && t.n < t.max; c++) t.s[t.n++] = *c;
    t.s[t.n] = 0;
    t.cur = t.n;
    t.shift = t.n ? 0 : 1; // a fresh name starts capitalized
    t.sym = false;
    t.done = done;
    app_screen(&text_screen);
}
