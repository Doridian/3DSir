// The UI shell, after Prox3DS's: a stack of screens, dialogs, toasts, the hardware job worker, menus,
// the log screen and the default bottom screen.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "log.h"

static const Screen* stack[16];
static int depth;
static u32 frame;
static bool quit;

static void show_top(void) {
    if(stack[depth - 1]->show) stack[depth - 1]->show();
}

void app_screen(const Screen* s) {
    if(depth && stack[depth - 1] == s) return;
    if(depth == 16) memmove(stack + 1, stack + 2, 14 * sizeof(*stack)); // keep the root
    else depth++;
    stack[depth - 1] = s;
    show_top();
}

const Screen* app_top(void) {
    return stack[depth - 1];
}

void app_back(void) {
    if(depth > 1) depth--;
    show_top();
}

void app_quit(void) {
    quit = true;
}

static char toast_msg[128];
static u32 toast_until;

void toast(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(toast_msg, sizeof(toast_msg), fmt, va);
    va_end(va);
    toast_until = frame + 180;
}

// ---------------------------------------------------------------- dialogs

enum { DLG_NONE, DLG_MSG, DLG_CONFIRM, DLG_POPUP };
static struct {
    int kind;
    char title[64], msg[512];
    void (*yes)(void);
    MenuItem items[16];
    int n;
    UiList l;
} dlg;

#define DLG_W 360

void msg_box(const char* title, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(dlg.msg, sizeof(dlg.msg), fmt, va);
    va_end(va);
    snprintf(dlg.title, sizeof(dlg.title), "%s", title);
    dlg.kind = DLG_MSG;
}

void confirm(const char* title, void (*yes)(void), const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(dlg.msg, sizeof(dlg.msg), fmt, va);
    va_end(va);
    snprintf(dlg.title, sizeof(dlg.title), "%s", title);
    dlg.yes = yes;
    dlg.kind = DLG_CONFIRM;
}

void popup(const char* title, const MenuItem* items, int n) {
    snprintf(dlg.title, sizeof(dlg.title), "%s", title);
    dlg.n = n < 16 ? n : 16;
    memcpy(dlg.items, items, dlg.n * sizeof(*items));
    dlg.l = (UiList){0};
    dlg.kind = DLG_POPUP;
}

static void dialog_input(u32 down, u32 repeat) {
    int kind = dlg.kind;
    if(kind == DLG_POPUP) {
        ui_list_nav(&dlg.l, dlg.n, dlg.n, repeat & (KEY_UP | KEY_DOWN));
        if(down & KEY_B) dlg.kind = DLG_NONE;
        if(down & KEY_A) {
            dlg.kind = DLG_NONE; // the item may open the next dialog
            dlg.items[dlg.l.sel].fn();
        }
        return;
    }
    if(down & (KEY_A | KEY_B)) {
        dlg.kind = DLG_NONE;
        if(kind == DLG_CONFIRM && (down & KEY_A)) dlg.yes();
    }
}

static void dialog_draw(void) {
    if(dlg.kind == DLG_POPUP) {
        float w = 240, h = 22 + dlg.n * LINE_H + 22, y = ui_box(w, h, dlg.title), x = (UI_W - w) / 2;
        for(int i = 0; i < dlg.n; i++, y += LINE_H) {
            if(i == dlg.l.sel) ui_sel(x + 2, y - 1, w - 4, LINE_H);
            ui_text(x + 10, y, S_BODY, ui_col[COL_TEXT], "%s", dlg.items[i].label);
        }
        ui_text(x + 10, y + 4, S_SMALL, ui_col[COL_DIM], BTN_A " select   " BTN_B " close");
        ui_hints(BTN_UD " item   " BTN_A " select   " BTN_B " close");
        return;
    }
    int lines = ui_wrap_lines(DLG_W - 20, S_SMALL, dlg.msg);
    float h = 22 + lines * SMALL_H + 26, y = ui_box(DLG_W, h, dlg.title), x = (UI_W - DLG_W) / 2;
    ui_text_wrap(x + 10, y, DLG_W - 20, S_SMALL, ui_col[COL_TEXT], dlg.msg);
    const char* hint = dlg.kind == DLG_CONFIRM ? BTN_A " yes   " BTN_B " no" : BTN_A " ok";
    ui_text(x + 10, y + lines * SMALL_H + 6, S_SMALL, ui_col[COL_DIM], "%s", hint);
    ui_hints(hint);
}

// ---------------------------------------------------------------- job worker

static Thread worker;
static volatile bool busy, finished;
static char job_title[64];
static void (*job_run)(void), (*job_done)(void);
static u32 job_flags;
static char note[256];
static LightLock note_lock;
volatile bool job_cancel;
volatile u32 ui_keys_held;

bool job_busy(void) {
    return busy;
}

void job_note(const char* fmt, ...) {
    char s[sizeof(note)];
    va_list va;
    va_start(va, fmt);
    vsnprintf(s, sizeof(s), fmt, va);
    va_end(va);
    LightLock_Lock(&note_lock);
    memcpy(note, s, sizeof(note));
    LightLock_Unlock(&note_lock);
}

static void job_thread(void* arg) {
    (void)arg;
    LightLock_Lock(&hw_lock);
    job_run();
    LightLock_Unlock(&hw_lock);
    finished = true;
}

void job_start(const char* title, void (*run)(void), void (*done)(void), u32 flags) {
    if(busy) return;
    snprintf(job_title, sizeof(job_title), "%s", title);
    job_run = run;
    job_done = done;
    job_flags = flags;
    job_cancel = false;
    finished = false;
    *note = 0;
    busy = true;
    if(!(flags & JOB_QUIET)) logp("--- %s ---\n", title);
    // Above the UI thread: learning times the IR receiver's bytes by polling, and the UI only gets in
    // between while the worker waits on I2C anyway. JOB_CPU jobs (downloading, indexing) go below it instead,
    // or the screen would freeze for minutes. 128 KB of stack for TLS (mbedtls) and libarchive.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    worker = threadCreate(job_thread, NULL, 0x20000, flags & JOB_CPU ? prio + 1 : 0x28, -2, false);
    if(!worker) {
        busy = false;
        msg_box("Error", "Could not start the worker thread.");
    }
}

static void job_reap(void) {
    if(!finished) return;
    threadJoin(worker, U64_MAX);
    threadFree(worker);
    worker = NULL;
    finished = busy = false;
    if(job_done) job_done();
}

static void busy_draw(void) {
    static const char spin[] = "|/-\\";
    char n[sizeof(note)];
    LightLock_Lock(&note_lock);
    memcpy(n, note, sizeof(n));
    LightLock_Unlock(&note_lock);
    if(job_flags & JOB_QUIET) { // a pill above the footer
        char t[96];
        snprintf(t, sizeof(t), "%c  %s", spin[(frame / 6) % 4], *n ? n : job_title);
        ui_fit(t, UI_W - 40, S_SMALL);
        float w = 20 + ui_text_width(S_SMALL, t);
        ui_rect((UI_W - w) / 2, UI_H - 44, w, 20, ui_col[COL_SEL]);
        ui_rect((UI_W - w) / 2, UI_H - 44, 2, 20, ui_col[COL_ACCENT]);
        ui_text_center(UI_W / 2, UI_H - 41, S_SMALL, ui_col[COL_TEXT], "%s", t);
        return;
    }
    float w = 372, h = 176;
    float y = ui_box(w, h, job_title), x = (UI_W - w) / 2, bottom = y - 22 + h;
    ui_text_right(x + w - 6, y - 21, S_BODY, ui_col[COL_ACCENT], "%c", spin[(frame / 8) % 4]);
    if(*n) y = ui_text_wrap(x + 10, y, w - 20, S_SMALL, ui_col[COL_TEXT], n) + 4;
    // The log's tail fills what is left above the hints line, the last line at the bottom.
    int lines = (int)((bottom - 20 - y) / 12);
    if(lines > 9) lines = 9;
    if(lines > 0) ui_rect(x + 6, y - 2, w - 12, lines * 12 + 4, ui_col[COL_BG]);
    int cnt = log_count(), rows = 0;
    char line[160];
    for(int i = cnt - 1; i >= 0 && rows < lines; i--) {
        log_line(i, line, sizeof(line));
        int k = ui_wrap_lines(w - 20, S_LOG, line);
        if(rows + k > lines) break;
        rows += k;
        ui_text_wrap(x + 10, y + 12 * (lines - rows), w - 20, S_LOG, ui_col[i == cnt - 1 ? COL_TEXT : COL_DIM], line);
    }
    if(job_flags & JOB_CANCEL)
        ui_text(x + 10, bottom - 16, S_SMALL, ui_col[job_cancel ? COL_WARN : COL_DIM], job_cancel ? "Stopping..." : BTN_B " stop");
}

// ---------------------------------------------------------------- log screen

#define LOG_ROWS 16
static int log_top = -1; // -1 = follow the end

// The first line of the last screenful: long lines wrap.
static int log_max(void) {
    int n = log_count(), rows = 0, top = n;
    char line[160];
    while(top > 0) {
        log_line(top - 1, line, sizeof(line));
        int k = ui_wrap_lines(UI_W - 12, S_LOG, line);
        if(rows + k > LOG_ROWS) break;
        rows += k;
        top--;
    }
    return top;
}

static void log_input(u32 down, u32 repeat) {
    int max = log_max(), top = log_top < 0 ? max : log_top;
    if(repeat & KEY_UP) top--;
    if(repeat & KEY_DOWN) top++;
    if(repeat & KEY_LEFT) top -= LOG_ROWS;
    if(repeat & KEY_RIGHT) top += LOG_ROWS;
    if(repeat & KEY_L) top = 0;
    if(repeat & KEY_R) top = max;
    top = top < 0 ? 0 : top > max ? max : top;
    log_top = top == max ? -1 : top;
    if(down & (KEY_B | KEY_SELECT)) app_back();
}

static void log_draw(void) {
    int n = log_count(), max = log_max(), top = log_top < 0 || log_top > max ? max : log_top;
    char hdr[48], line[160];
    snprintf(hdr, sizeof(hdr), "%s%d lines", log_top < 0 ? "following, " : "", n);
    ui_header("Log", hdr);
    for(int i = top, rows = 0; i < n; i++) {
        log_line(i, line, sizeof(line));
        int k = ui_wrap_lines(UI_W - 12, S_LOG, line);
        if(rows + k > LOG_ROWS) break;
        ui_text_wrap(6, 23 + rows * 12, UI_W - 12, S_LOG, ui_col[COL_TEXT], line);
        rows += k;
    }
    ui_footer(BTN_UD " scroll   " BTN_LR " page   " BTN_L "/" BTN_R " start/end   " BTN_B " back");
}

static const Screen log_screen = {NULL, log_input, log_draw, NULL};

void app_open_log(void) {
    log_top = -1;
    app_screen(&log_screen);
}

// ---------------------------------------------------------------- menus

void menu_input(Menu* m, u32 down, u32 repeat) {
    ui_list_nav(&m->l, m->n, m->n, repeat & (KEY_UP | KEY_DOWN));
    if(down & KEY_A) m->entries[m->l.sel].fn();
    else if((down & KEY_B) && depth > 1) app_back();
}

void menu_draw(Menu* m) {
#ifdef APP_RELEASE
    ui_header(m->title, NULL);
#else
    ui_header(m->title, *net_addr ? net_addr : "no network");
#endif
    for(int i = 0; i < m->n; i++) {
        float y = 30 + i * 20;
        if(i == m->l.sel) ui_sel(8, y - 3, 180, 19);
        ui_text(20, y, S_BODY, ui_col[i == m->l.sel ? COL_TEXT : COL_DIM], "%s", m->entries[i].label);
    }
    ui_rect(200, 27, UI_W - 208, 140, ui_col[COL_PANEL]);
    ui_text_wrap(208, 32, UI_W - 224, S_SMALL, ui_col[COL_TEXT], m->entries[m->l.sel].help);
    ui_footer(depth > 1 ? BTN_A " select   " BTN_B " back   SELECT log   START exit" : BTN_A " select   SELECT log   START exit");
}

// ---------------------------------------------------------------- bottom screen

static u32 touch_keys; // pressed on the bottom screen, delivered next frame

void app_press(u32 keys) {
    touch_keys |= keys;
}

// U+E000.. button glyphs in a hints string -> keys (0: the D-pad ones, which the D-pad covers)
static u32 glyph_key(const u8* p) {
    if(p[0] != 0xEE) return ~0u;
    u32 cp = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    switch(cp) {
    case 0xE000: return KEY_A;
    case 0xE001: return KEY_B;
    case 0xE002: return KEY_X;
    case 0xE003: return KEY_Y;
    case 0xE004: return KEY_L;
    case 0xE005: return KEY_R;
    case 0xE006:
    case 0xE07D:
    case 0xE07E: return 0;
    }
    return ~0u;
}

#define BOT_LOG 7 // log lines on the default bottom screen

// The default: the log's tail, a D-pad and one button per footer hint.
static void bottom_draw(void) {
    int n = log_count();
    ui_rect(0, 0, UI_BW, BOT_LOG * 12 + 6, ui_col[COL_PANEL]);
    ui_rect(0, BOT_LOG * 12 + 5, UI_BW, 1, ui_col[COL_ACCENT]);
    for(int i = 0; i < BOT_LOG; i++) {
        int k = n - BOT_LOG + i;
        if(k < 0) continue;
        char line[160];
        log_line(k, line, sizeof(line));
        ui_text(4, 3 + i * 12, S_LOG, ui_col[k == n - 1 ? COL_TEXT : COL_DIM], "%s", ui_fit(line, UI_BW - 8, S_LOG));
    }
    float y0 = BOT_LOG * 12 + 14, c = 36; // D-pad cell
    u32 pad = ui_col[COL_BAR];
    if(ui_button_rep(c, y0, c, c, pad, "")) app_press(KEY_UP);
    if(ui_button_rep(0, y0 + c, c, c, pad, "")) app_press(KEY_LEFT);
    if(ui_button_rep(2 * c, y0 + c, c, c, pad, "")) app_press(KEY_RIGHT);
    if(ui_button_rep(c, y0 + 2 * c, c, c, pad, "")) app_press(KEY_DOWN);
    // the hints: "<glyph> label   <glyph> label ...", two columns right of the D-pad
    const u8* h = (const u8*)ui_hints_now();
    float bx = 3 * c + 8, bw = (UI_BW - bx - 4) / 2 - 2, bh = 30;
    int i = 0;
    while(*h) {
        u32 key = glyph_key(h);
        if(key == ~0u) {
            h++;
            continue;
        }
        const u8* e = h + 3;
        while(*e && glyph_key(e) == ~0u) e++;
        char label[48];
        int len = e - h < (int)sizeof(label) - 1 ? e - h : (int)sizeof(label) - 1;
        memcpy(label, h, len);
        label[len] = 0;
        char* gap = strstr(label + 3, "   "); // hints end at a wide gap ("... SELECT log" isn't a button)
        if(gap) *gap = 0;
        for(len = strlen(label); len > 3 && label[len - 1] == ' ';) label[--len] = 0;
        h = e;
        if(!key) continue;
        float x = bx + (i % 2) * (bw + 4), y = y0 + (i / 2) * (bh + 4);
        if(y + bh > UI_H - 2) break;
        if(ui_button(x, y, bw, bh, ui_col[COL_BAR], label)) app_press(key);
        i++;
    }
    if(ui_button(0, UI_H - 26, 3 * c, 24, ui_col[COL_PANEL], "Log")) app_press(KEY_SELECT);
}

// ---------------------------------------------------------------- loop

static bool running(void) {
    return !quit && !restart_requested && aptMainLoop();
}

void app_run(const Screen* root) {
    LightLock_Init(&note_lock);
    hidSetRepeatParameters(18, 4);
    app_screen(root);
    while(running()) {
        frame++;
        hidScanInput();
        u32 ext = touch_keys, tap = 0;
#ifndef APP_RELEASE
        ext |= __atomic_exchange_n(&net_keys, 0, __ATOMIC_SEQ_CST);
        tap = __atomic_exchange_n(&net_touch, 0, __ATOMIC_SEQ_CST);
#endif
        touch_keys = 0;
        u32 down = hidKeysDown() | ext, repeat = hidKeysDownRepeat() | ext;
        ui_keys_held = hidKeysHeld() | ext;
        // touch: a new tap, then after a delay repeats while held (D-pad and keyboard keys)
        static u32 held;
        touchPosition tp = {0};
        if(hidKeysHeld() & KEY_TOUCH) hidTouchRead(&tp), held++;
        else held = 0;
        if(tap) ui_touch(true, false, tap & 0xFFFF, (tap >> 16) & 0x7FFF);
        else ui_touch(held == 1 || (held > 24 && held % 5 == 0), held > 1, tp.px, tp.py);
        job_reap();
        if((down & KEY_START) && !busy) break;
        if(busy) {
            if((down & KEY_B) && (job_flags & JOB_CANCEL)) job_cancel = true;
        } else if(dlg.kind) {
            dialog_input(down, repeat);
        } else if((down & KEY_SELECT) && stack[depth - 1] != &log_screen) {
            app_open_log();
        } else {
            stack[depth - 1]->input(down, repeat);
        }
        if(quit) break;

        ui_frame_begin();
        stack[depth - 1]->draw();
        bool box = busy && !(job_flags & JOB_QUIET);
        if(busy) busy_draw();
        if(dlg.kind && !box) dialog_draw();
        if(box) ui_hints(job_flags & JOB_CANCEL ? BTN_B " stop" : "");
        if(frame < toast_until && !box && !dlg.kind) {
            char t[128];
            snprintf(t, sizeof(t), "%s", toast_msg);
            ui_fit(t, UI_W - 32, S_SMALL);
            float w = 16 + ui_text_width(S_SMALL, t);
            ui_rect((UI_W - w) / 2, UI_H - 44, w, 20, ui_col[COL_BAR]);
            ui_rect((UI_W - w) / 2, UI_H - 44, 2, 20, ui_col[COL_CURSOR]);
            ui_text_center(UI_W / 2, UI_H - 41, S_SMALL, ui_col[COL_TEXT], "%s", t);
        }
        ui_bottom();
        if(!busy && !dlg.kind && stack[depth - 1]->bottom) stack[depth - 1]->bottom();
        else bottom_draw();
        ui_frame_end();
    }
    // A job still running: let it finish (a send ends on its own, B-able ones are told to stop).
    job_cancel = true;
    for(int i = 0; i < 100 && busy && !finished; i++) svcSleepThread(50000000LL);
    if(finished) job_reap();
}
