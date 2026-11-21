#pragma once

#include <3ds.h>
#include <stdbool.h>

#include "irenc.h"
#include "irfile.h"
#include "ui.h"

#define ROOT_DIR "sdmc:/3ds/3dsir"
#define SETTINGS_PATH ROOT_DIR "/.settings" // dotfiles don't show in the browser

// ---------------------------------------------------------------- settings (main.c)

extern IrCarrierStyle style; // dense (default) or clean
extern unsigned repeats; // every press sends the signal this many times
void settings_save(void);

// ---------------------------------------------------------------- hardware (irsend.c)

// Held by whoever touches the IR hardware: jobs and the net server's requests.
extern LightLock hw_lock;

extern char app_path[256]; // the running .3dsx (argv[0])
extern volatile bool restart_requested; // exit the UI and relaunch app_path (net self-update)

Result iru_open(void);
void iru_close(void);
bool iru_is_open(void);

// ir:u path (hw_lock held). Fails while the hardware is taken over.
Result app_send(const IrTimings* tm, IrCarrierStyle style, u32* bytes);
// Raw bytes through ir:u at one IRU_SetBitRate value; *us = time until on air.
Result app_iru_send(u8 bitrate, const u8* data, u32 len, u32* us);
// Bytes through ir:u at a carrier's bit rate (hw_lock held).
Result app_send_bytes(const IrCarrier* c, const u8* data, size_t n);
// Sends every bit rate a fixed amount of data and logs the time it took (hw_lock held).
void app_timing_check(volatile bool* cancel);
Result app_set_led(bool on);

// Direct hardware: close ir:u and take the SC16IS750 (hw_lock held).
Result app_hw_direct(u32 flags);
extern volatile bool trim_overridden; // the net server set ir_mark_trim_us: use it for every style
// Give the hardware back and reopen ir:u (hw_lock held).
void app_hw_iru(void);

// Learning (hw_lock held, hardware taken over): sets the UART up for IrDA reception at 38400 baud and
// records when each burst of carrier arrives (microseconds from the first one) until 250 ms of quiet
// after one, max edges, or *cancel. Returns how many.
size_t app_learn_capture(u32* t, size_t max, volatile bool* cancel);

// ---------------------------------------------------------------- the UI shell (shell.c)

typedef struct {
    void (*show)(void); // whenever it becomes the top screen (entered, or returned to); may be NULL
    void (*input)(u32 down, u32 repeat);
    void (*draw)(void);
    void (*bottom)(void); // its own bottom screen; NULL = the footer's hints as buttons, a D-pad, the log
} Screen;
void app_press(u32 keys); // from a bottom screen: keys pressed next frame, as if from the buttons
void app_screen(const Screen* s); // push (nothing if it is on top already)
void app_back(void); // pop to the screen it was entered from
const Screen* app_top(void);
void app_run(const Screen* root); // the UI loop, returns on exit
void app_quit(void);
void app_open_log(void);

// Dialogs on top of the current screen (one at a time).
void msg_box(const char* title, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void confirm(const char* title, void (*yes)(void), const char* fmt, ...) __attribute__((format(printf, 3, 4)));
typedef struct {
    const char* label;
    void (*fn)(void);
} MenuItem;
void popup(const char* title, const MenuItem* items, int n);
void toast(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // a short note at the bottom

// Runs run() on a worker thread (hw_lock held), then done() on the UI thread. JOB_CANCEL: B sets
// job_cancel. JOB_QUIET: a small "title..." note instead of the busy box (short sends).
enum { JOB_CANCEL = 1, JOB_QUIET = 2 };
void job_start(const char* title, void (*run)(void), void (*done)(void), u32 flags);
bool job_busy(void);
extern volatile bool job_cancel;
extern volatile u32 ui_keys_held; // the buttons held this frame (a job repeats a send while A is)
void job_note(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // the busy box's text, any thread

// A menu screen: entries on the left, the selected one's help on the right.
typedef struct {
    const char *label, *help;
    void (*fn)(void);
} MenuEntry;
typedef struct {
    const char* title;
    const MenuEntry* entries;
    int n;
    UiList l;
} Menu;
void menu_input(Menu* m, u32 down, u32 repeat);
void menu_draw(Menu* m);

// ---------------------------------------------------------------- text entry (entry.c)

// The bottom-screen keyboard, as in Prox3DS. done gets the text (possibly empty) after the entry
// screen is gone, so it may open the next one; B cancels without calling it.
void entry_text(const char* title, const char* what, const char* init, u32 max, void (*done)(const char* s));

// ---------------------------------------------------------------- screens (screens.c)

const Screen* screens_root(void);

#ifndef APP_RELEASE
// netsrv.c
#define NET_PORT 8020
extern char net_addr[32];
extern volatile u32 net_keys; // KEY_* pressed remotely, consumed by the UI's next input scan
extern volatile u32 net_touch; // a tap remotely: 0x80000000 | y << 16 | x, consumed likewise
void net_start(void);
void net_stop(void);
#endif
