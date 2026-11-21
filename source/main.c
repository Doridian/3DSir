#include <3ds.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "irenc.h"
#include "irfile.h"
#include "irhw.h"
#include "log.h"

#define IRU_MAX_TRANSFER 0x800
#define LOG_PATH "sdmc:/3dsir.log" // debug build only
#define LIST_ROWS 26

static PrintConsole top, bottom;

static u8 cur_bitrate;
static bool led_state;
static IrCarrierStyle style = IrCarrierDense;
static unsigned repeats = 3;
static u8* txbuf;
static size_t txbuf_cap;
static Handle send_event;
static u32* iru_shm;
static bool iru_ok; // ir:u open (false while the hardware is taken over)

LightLock hw_lock;
char app_path[256] = "sdmc:/3ds/3dsir.3dsx";
volatile bool restart_requested;

// ---------------------------------------------------------------- input

// Buttons this frame: the real ones plus any the debug build's net server pressed.
static u32 keys_down, keys_held;

static void scan_input(void) {
    hidScanInput();
    u32 net = 0;
#ifndef APP_RELEASE
    net = __atomic_exchange_n(&net_keys, 0, __ATOMIC_SEQ_CST);
#endif
    keys_down = hidKeysDown() | net;
    keys_held = hidKeysHeld() | net;
}

static bool running(void) {
    return !restart_requested && aptMainLoop();
}

// ---------------------------------------------------------------- ir:u

static Result iru_open(void) {
    if(iru_ok) return 0;
    if(!iru_shm) iru_shm = memalign(0x1000, 0x1000);
    Result rc = -1;
    // Right after a relaunch the module may not have registered ir:u yet.
    for(int t = 0; t < 20; t++) {
        rc = iruInit(iru_shm, 0x1000);
        if(R_SUCCEEDED(rc)) break;
        svcSleepThread(100000000LL);
    }
    logp("iruInit: %08lX\n", rc);
    if(R_FAILED(rc)) return rc;
    IRU_GetBitRate(&cur_bitrate);
    rc = IRU_GetSendFinishedEvent(&send_event);
    if(R_FAILED(rc)) logp("GetSendFinishedEvent: %08lX\n", rc);
    iru_ok = true;
    return 0;
}

static void iru_close(void) {
    if(!iru_ok) return;
    svcCloseHandle(send_event);
    send_event = 0;
    iruExit();
    iru_ok = false;
}

Result app_hw_direct(void) {
    iru_close();
    Result rc = irhw_takeover();
    // While the ir module is gone the HOME menu would hang waiting for it.
    aptSetHomeAllowed(false);
    aptSetSleepAllowed(false);
    return rc;
}

void app_hw_iru(void) {
    irhw_give_back(); // also when the takeover failed halfway: the module may be stopped already
    iru_open();
    aptSetHomeAllowed(true);
    aptSetSleepAllowed(true);
}

// ---------------------------------------------------------------- sending

static Result iru_get_transfer_state(u8* out) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0xF, 0, 0);
    Result rc = svcSendSyncRequest(iruGetServHandle());
    if(R_FAILED(rc)) return rc;
    *out = cmdbuf[2];
    return cmdbuf[1];
}

// What happened during the last send_chunk, for on-screen diagnostics.
static struct {
    u8 state_before; // transfer state before starting (1 = ready)
    s32 event_ms; // when the send-finished event fired, -1 if it never did
    u32 expected_ms; // on-air time of the data
    bool stuck; // state still "sending" well after the data must be out
} last_send;

static u32 ms_since(u64 t0) {
    return (svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11;
}

// Sends one transfer and blocks until it has gone out on air. This must not
// use IRU_WaitSendTransfer (or iruSendData with wait) up front: that command
// *cancels* the transfer in progress, which cut every signal off after ~10 ms.
static Result send_chunk(const u8* data, size_t len, u32 baud) {
    u8 state = 0;
    iru_get_transfer_state(&state);
    last_send.state_before = state;
    if(state == 2) IRU_WaitSendTransfer(); // a previous transfer never settled

    svcClearEvent(send_event);
    u64 t0 = svcGetSystemTick();
    Result rc = IRU_StartSendTransfer((u8*)data, len);
    if(R_FAILED(rc)) return rc;

    last_send.expected_ms = (u64)len * 10 * 1000 / baud;
    u32 limit_ms = last_send.expected_ms + 200;
    rc = svcWaitSynchronization(send_event, (s64)limit_ms * 1000000LL);
    last_send.event_ms = R_SUCCEEDED(rc) ? (s32)ms_since(t0) : -1;

    // Don't trust the event alone; 1 = ready, 2 = sending. The UART cannot be
    // slower than its bit rate, so once the limit has passed all data is out
    // and the transfer is only stuck in the sysmodule's bookkeeping: cancel it.
    last_send.stuck = false;
    while(R_SUCCEEDED(iru_get_transfer_state(&state)) && state == 2) {
        if(ms_since(t0) > limit_ms) {
            last_send.stuck = true;
            IRU_WaitSendTransfer();
            break;
        }
        svcSleepThread(1000000LL);
    }
    return 0;
}

// Splits a long byte stream into ir:u sized transfers. Each split adds ~10 ms
// of silence, so cut in the middle of the longest space (run of space_byte)
// where the extra delay only stretches a gap the receiver already ignores.
static Result send_bytes(const u8* data, size_t n, u32 baud, u8 space_byte) {
    size_t off = 0;
    while(off < n) {
        size_t len = n - off;
        if(len > IRU_MAX_TRANSFER) {
            size_t best_start = 0, best_len = 0, run = 0;
            for(size_t i = 1; i <= IRU_MAX_TRANSFER; i++) {
                run = data[off + i - 1] == space_byte ? run + 1 : 0;
                if(run > best_len) {
                    best_len = run;
                    best_start = i - run;
                }
            }
            len = best_len ? best_start + best_len / 2 + 1 : IRU_MAX_TRANSFER;
        }
        Result rc = send_chunk(data + off, len, baud);
        if(R_FAILED(rc)) return rc;
        off += len;
    }
    return 0;
}

static Result set_carrier(const IrCarrier* c) {
    if(c->iru_value == cur_bitrate) return 0;
    Result rc = IRU_SetBitRate(c->iru_value);
    if(R_SUCCEEDED(rc)) cur_bitrate = c->iru_value;
    return rc;
}

// Sends a waveform through ir:u (hw_lock held). *bytes = UART bytes rendered.
static Result send_timings_locked(const IrTimings* tm, IrCarrierStyle st, u32* bytes) {
    if(!iru_ok) return -2; // the hardware is taken over
    const IrCarrier* c = ir_pick_carrier(tm->freq, st);
    Result rc = set_carrier(c);
    if(R_FAILED(rc)) return rc;

    size_t n = ir_render_uart(tm, c, NULL, 0);
    if(n > txbuf_cap) {
        free(txbuf);
        txbuf_cap = n;
        txbuf = malloc(n);
    }
    ir_render_uart(tm, c, txbuf, n);
    if(bytes) *bytes = n;
    return send_bytes(txbuf, n, c->baud, ir_space_byte(c));
}

Result app_send(const IrTimings* tm, IrCarrierStyle st, u32* bytes) {
    return send_timings_locked(tm, st, bytes); // the net server holds hw_lock already
}

static Result send_timings(const IrTimings* tm) {
    LightLock_Lock(&hw_lock);
    Result rc = send_timings_locked(tm, style, NULL);
    LightLock_Unlock(&hw_lock);
    return rc;
}

static const u32 iru_nominal[19] = {[3] = 115200, 96000, 72000, 48000, 36000, 24000, 18000, 12000,
                                    9600, 6000, 3000, 57600, 38400, 19200, 7200, 4800};

Result app_iru_send(u8 bitrate, const u8* data, u32 len, u32* us) {
    if(!iru_ok) return -2;
    if(bitrate < 3 || bitrate > 18 || !len) return -3;
    Result rc = IRU_SetBitRate(bitrate);
    cur_bitrate = R_SUCCEEDED(rc) ? bitrate : 0;
    if(R_FAILED(rc)) return rc;
    u64 t0 = svcGetSystemTick();
    rc = send_bytes(data, len, iru_nominal[bitrate], 0xFF);
    *us = irhw_us(svcGetSystemTick() - t0);
    return rc;
}

static void carrier_test(void) {
    static u8 carrier[IRU_MAX_TRANSFER];
    const IrCarrier* c = ir_pick_carrier(38000, style);
    memset(carrier, c->cycle_slots == 2 ? 0x55 : 0x00, sizeof(carrier));
    logp("Carrier test %lu Hz: hold A (B to exit)\n", c->baud / c->cycle_slots);
    logp("Look at the IR port through a phone camera.\n");

    while(running()) {
        scan_input();
        u32 held = keys_held;
        if(keys_down & KEY_B) break;
        if(held & KEY_A) {
            LightLock_Lock(&hw_lock);
            Result rc = iru_ok ? set_carrier(c) : -2;
            if(R_SUCCEEDED(rc)) rc = send_bytes(carrier, sizeof(carrier), c->baud, ir_space_byte(c));
            LightLock_Unlock(&hw_lock);
            if(R_FAILED(rc)) {
                logp("send: %08lX\n", rc);
                svcSleepThread(500000000LL);
            }
        } else {
            gspWaitForVBlank();
        }
    }
}

// Sends a fixed amount of data at every bit rate and times it. If ir:u keeps
// the UART busy the whole time, the measured rate matches the nominal one; a
// lower rate means gaps between bytes (or that SetBitRate did not take).
static void timing_check(void) {
    const u32* nominal = iru_nominal;
    static u8 data[IRU_MAX_TRANSFER];
    consoleSelect(&bottom);
    consoleClear();
    LightLock_Lock(&hw_lock);
    if(!iru_ok) logp("ir:u is closed (hardware taken over)\n");
    logp("val nominal  got   ms  measured\n");
    for(u8 v = 3; v <= 18 && iru_ok && running(); v++) {
        size_t n = nominal[v] >= 36000 ? sizeof(data) : 256;
        Result rc = IRU_SetBitRate(v);
        u8 got = 0;
        IRU_GetBitRate(&got);
        u64 t0 = svcGetSystemTick();
        if(R_SUCCEEDED(rc)) rc = send_chunk(data, n, nominal[v]);
        u64 ticks = svcGetSystemTick() - t0;
        if(R_FAILED(rc)) {
            logp("%3u %7lu %4u  err %08lX\n", v, nominal[v], got, rc);
            continue;
        }
        u32 us = ticks * 1000000 / SYSCLOCK_ARM11;
        logp("%3u %7lu %4u %4lu %8llu\n", v, nominal[v], got, us / 1000,
               (u64)n * 10 * 1000000 / (us ? us : 1));
    }
    cur_bitrate = 0; // force a SetBitRate on the next send
    LightLock_Unlock(&hw_lock);
    logp("B to go back\n");
    while(running()) {
        gspWaitForVBlank();
        scan_input();
        if(keys_down & KEY_B) break;
    }
}

// ---------------------------------------------------------------- lists

static void adjust_repeats(u32 down) {
    unsigned old = repeats;
    if((down & KEY_L) && repeats > 1) repeats--;
    if((down & KEY_R) && repeats < 10) repeats++;
    if(repeats != old) {
        logp("Repeats: %u\n", repeats);
    }
}

static void draw_list(const char* title, char** items, size_t n, size_t sel, const char* help) {
    consoleSelect(&top);
    consoleClear();
    printf("\x1b[1;1H\x1b[36m%.49s\x1b[0m\n", title);
    size_t first = sel >= LIST_ROWS / 2 ? sel - LIST_ROWS / 2 : 0;
    if(n > LIST_ROWS && first > n - LIST_ROWS) first = n - LIST_ROWS;
    for(size_t i = first; i < n && i < first + LIST_ROWS; i++)
        printf("%s%c %.46s\x1b[0m\n", i == sel ? "\x1b[33m" : "", i == sel ? '>' : ' ', items[i]);
    printf("\x1b[29;1H\x1b[36m%.49s\x1b[0m", help);
}

static int move_sel(u32 down, size_t n, size_t* sel) {
    if(!n) return 0;
    size_t old = *sel;
    if(down & KEY_DOWN) *sel = (*sel + 1) % n;
    if(down & KEY_UP) *sel = (*sel + n - 1) % n;
    if(down & KEY_RIGHT) *sel = *sel + 10 < n ? *sel + 10 : n - 1;
    if(down & KEY_LEFT) *sel = *sel >= 10 ? *sel - 10 : 0;
    return old != *sel;
}

static void report(const IrSignal* s, Result rc, const IrTimings* tm) {
    const IrCarrier* c = ir_pick_carrier(tm->freq, style);
    if(s->raw)
        logp("%.20s raw %luHz", s->name, s->frequency);
    else
        logp("%.20s %s %lX/%lX", s->name, s->protocol, s->address, s->command);
    if(R_FAILED(rc))
        logp(" \x1b[31m%08lX\x1b[0m\n", rc);
    else
        logp(" %uB @%lu\n", ir_render_uart(tm, c, NULL, 0), c->baud);
    if(R_SUCCEEDED(rc))
        logp("  pre %u ev %ld/%lu ms%s\n", last_send.state_before, last_send.event_ms,
               last_send.expected_ms, last_send.stuck ? " \x1b[33mstuck\x1b[0m" : "");
}

// Sends every signal named like `name` (e.g. all "Power" codes in a universal
// library), one after another, until done or B is pressed.
static void sweep(IrFile* f, const char* name, IrTimings* tm) {
    logp("\x1b[32mSweeping \"%s\", B to stop\x1b[0m\n", name);
    size_t count = 0;
    for(size_t i = 0; i < f->n && running(); i++) {
        scan_input();
        if(keys_down & KEY_B) {
            logp("stopped\n");
            return;
        }
        if(strcmp(f->sig[i].name, name)) continue;
        if(!irfile_signal_timings(&f->sig[i], repeats, tm)) continue;
        Result rc = send_timings(tm);
        report(&f->sig[i], rc, tm);
        count++;
        svcSleepThread(150000000LL); // let the TV settle between codes
    }
    logp("sent %zu codes\n", count);
}

static void signal_menu(IrFile* f, const char* title) {
    char** items = malloc(f->n * sizeof(char*));
    for(size_t i = 0; i < f->n; i++) items[i] = f->sig[i].name;
    size_t sel = 0;
    IrTimings tm;
    ir_timings_init(&tm);
    draw_list(title, items, f->n, sel, "A send  Y sweep same name  B back");

    while(running()) {
        gspWaitForVBlank();
        scan_input();
        u32 down = keys_down;
        if(down & KEY_B) break;
        if(move_sel(down, f->n, &sel)) draw_list(title, items, f->n, sel, "A send  Y sweep same name  B back");
        adjust_repeats(down);
        if(!f->n) continue;

        if(down & KEY_A) {
            IrSignal* s = &f->sig[sel];
            if(!irfile_signal_timings(s, repeats, &tm)) {
                logp("unsupported: %s\n", s->raw ? "raw" : s->protocol);
                continue;
            }
            // Keep resending the same waveform (same RC5/RC6 toggle bit) while held.
            do {
                Result rc = send_timings(&tm);
                report(s, rc, &tm);
                if(R_FAILED(rc)) break;
                svcSleepThread(40000000LL);
                scan_input();
            } while(keys_held & KEY_A);
        }
        if(down & KEY_Y) {
            sweep(f, f->sig[sel].name, &tm);
            draw_list(title, items, f->n, sel, "A send  Y sweep same name  B back");
        }
    }
    ir_timings_free(&tm);
    free(items);
}

// ---------------------------------------------------------------- built-ins

static const struct {
    const char* name;
    const char* protocol;
    u32 address, command;
} builtin[] = {
    {"Samsung Vol+", "Samsung32", 0x07, 0x07},
    {"Samsung Power", "Samsung32", 0x07, 0x02},
    {"LG Vol+", "NEC", 0x04, 0x02},
    {"LG Power", "NEC", 0x04, 0x08},
    {"Sony Vol+", "SIRC", 0x01, 0x12},
    {"Sony Power", "SIRC", 0x01, 0x15},
    {"Philips Vol+", "RC5", 0x00, 0x10},
    {"Philips Power", "RC5", 0x00, 0x0C},
};

static void builtin_menu(void) {
    IrFile f;
    f.n = sizeof(builtin) / sizeof(builtin[0]);
    f.sig = calloc(f.n, sizeof(IrSignal));
    for(size_t i = 0; i < f.n; i++) {
        snprintf(f.sig[i].name, sizeof(f.sig[i].name), "%s", builtin[i].name);
        snprintf(f.sig[i].protocol, sizeof(f.sig[i].protocol), "%s", builtin[i].protocol);
        f.sig[i].address = builtin[i].address;
        f.sig[i].command = builtin[i].command;
    }
    signal_menu(&f, "Built-in test codes");
    irfile_free(&f);
}

// ---------------------------------------------------------------- browser

static int cmp_str(const void* a, const void* b) {
    return strcasecmp(*(char* const*)a, *(char* const*)b);
}

static size_t list_dir(const char* path, char*** out) {
    size_t n = 0, cap = 16;
    char** items = malloc(cap * sizeof(char*));
    DIR* d = opendir(path);
    struct dirent* e;
    while(d && (e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        bool dir = e->d_type == DT_DIR;
        size_t len = strlen(e->d_name);
        if(!dir && (len < 3 || strcasecmp(e->d_name + len - 3, ".ir"))) continue;
        if(n == cap) items = realloc(items, (cap *= 2) * sizeof(char*));
        items[n] = malloc(len + 2);
        sprintf(items[n++], "%s%s", e->d_name, dir ? "/" : "");
    }
    if(d) closedir(d);
    qsort(items, n, sizeof(char*), cmp_str);
    *out = items;
    return n;
}

#ifdef APP_RELEASE
#define ROOT_EXTRA 3
#else
#define ROOT_EXTRA 4

// Takes the SC16IS750 over (the ir module stops), logs its registers and the I2C round-trip
// time, then counts what the RX FIFO receives until B: point a remote at the port.
static void chip_diag(void) {
    consoleSelect(&bottom);
    consoleClear();
    logp("\x1b[36mIR chip diagnostic\x1b[0m\n");
    LightLock_Lock(&hw_lock);
    Result rc = app_hw_direct();
    if(R_SUCCEEDED(rc)) {
        irhw_dump();
        u32 mn, avg, mx;
        rc = irhw_bench(200, &mn, &avg, &mx);
        logp("I2C reg read: min %lu avg %lu max %lu us (%08lX)\n", mn, avg, mx, rc);
    } else {
        logp("takeover failed: %08lX\n", rc);
    }
    LightLock_Unlock(&hw_lock);

    logp("RX: point a remote at the port. B: give back\n");
    u32 bytes = 0, polls = 0;
    u64 t0 = svcGetSystemTick();
    while(running()) {
        scan_input();
        if(keys_down & KEY_B) break;
        LightLock_Lock(&hw_lock);
        u8 lvl = 0, buf[64];
        if(irhw_owned() && R_SUCCEEDED(sc_get(SC_RXLVL, &lvl)) && lvl) {
            if(lvl > 64) lvl = 64;
            sc_read(SC_RHR, buf, lvl);
            bytes += lvl;
        }
        LightLock_Unlock(&hw_lock);
        polls++;
        if(!irhw_owned()) svcSleepThread(16000000LL);
        if(irhw_us(svcGetSystemTick() - t0) >= 500000) {
            if(bytes) logp("RX %lu bytes in 0.5 s (%lu polls)\n", bytes, polls);
            bytes = polls = 0;
            t0 = svcGetSystemTick();
        }
    }

    LightLock_Lock(&hw_lock);
    app_hw_iru();
    LightLock_Unlock(&hw_lock);
    logp("ir module back\n");
}
#endif

static void browse(const char* path, bool is_root) {
    char** names;
    size_t n = list_dir(path, &names);
    size_t extra = is_root ? ROOT_EXTRA : 0;
    char** items = malloc((n + extra) * sizeof(char*));
    if(is_root) {
        items[0] = "[Built-in test codes]";
        items[1] = "[Carrier test]";
        items[2] = "[Bit rate / timing check]";
#ifndef APP_RELEASE
        items[3] = "[IR chip diagnostic]";
#endif
    }
    memcpy(items + extra, names, n * sizeof(char*));
    size_t total = n + extra, sel = 0;
    const char* help = is_root ? "A open X IR LED SELECT carrier START exit" : "A open  B back";
    char title[64];
    snprintf(title, sizeof(title), "%s", path);
#ifndef APP_RELEASE
    if(is_root && *net_addr) snprintf(title, sizeof(title), "%s  net %s", path, net_addr);
#endif
    draw_list(title, items, total, sel, help);

    while(running()) {
        gspWaitForVBlank();
        scan_input();
        u32 down = keys_down;
        if(is_root && (down & KEY_START)) break;
        if(!is_root && (down & KEY_B)) break;
        if(move_sel(down, total, &sel)) draw_list(title, items, total, sel, help);

        adjust_repeats(down);
        if(down & KEY_SELECT) {
            style = style == IrCarrierClean ? IrCarrierDense : IrCarrierClean;
            logp("Carrier style: %s\n", style == IrCarrierClean ? "clean (0x55, 36/48/57.6k)" : "dense (0x00, 36/38.4k)");
        }
        if(down & KEY_X) {
            led_state = !led_state;
            LightLock_Lock(&hw_lock);
            Result rc = iru_ok ? IRU_SetIRLEDState(led_state) : -2;
            LightLock_Unlock(&hw_lock);
            logp("SetIRLEDState(%d): %08lX\n", led_state, rc);
        }
        if(!(down & KEY_A) || !total) continue;

        if(is_root && sel == 0) {
            builtin_menu();
        } else if(is_root && sel == 1) {
            carrier_test();
        } else if(is_root && sel == 2) {
            timing_check();
#ifndef APP_RELEASE
        } else if(is_root && sel == 3) {
            chip_diag();
#endif
        } else {
            char sub[512];
            const char* name = items[sel];
            snprintf(sub, sizeof(sub), "%s/%s", path, name);
            size_t len = strlen(sub);
            if(sub[len - 1] == '/') {
                sub[len - 1] = 0;
                browse(sub, false);
            } else {
                IrFile f;
                if(irfile_load(sub, &f)) {
                    logp("%s: %zu signals\n", name, f.n);
                    signal_menu(&f, name);
                    irfile_free(&f);
                } else {
                    logp("cannot open %s\n", sub);
                }
            }
        }
        draw_list(title, items, total, sel, help);
    }

    for(size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    free(items);
}

// Relaunch the (updated) .3dsx the way the Homebrew Menu starts one under Luma: tell Rosalina's
// hb:ldr the target and its argv, then soft-reset into it. Without hb:ldr this is a plain exit.
static void relaunch_self(void) {
    Handle hbldr;
    if(R_FAILED(svcConnectToPort(&hbldr, "hb:ldr"))) return;
    const char* path = strncmp(app_path, "sdmc:/", 6) ? app_path : app_path + 5; // SD-relative
    static u32 args[0x400 / 4]; // argc, then the NUL-separated strings
    args[0] = 1;
    strcpy((char*)(args + 1), app_path);
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2); // SetTarget
    cmd[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmd[2] = (u32)path;
    Result rc = svcSendSyncRequest(hbldr);
    if(R_SUCCEEDED(rc)) rc = cmd[1];
    if(R_SUCCEEDED(rc)) {
        cmd[0] = IPC_MakeHeader(3, 0, 2); // SetArgv
        cmd[1] = IPC_Desc_StaticBuffer(sizeof(args), 1);
        cmd[2] = (u32)args;
        rc = svcSendSyncRequest(hbldr);
        if(R_SUCCEEDED(rc)) rc = cmd[1];
    }
    svcCloseHandle(hbldr);
    if(R_SUCCEEDED(rc)) aptSetChainloaderToSelf();
}

int main(int argc, char* argv[]) {
    if(argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6)) snprintf(app_path, sizeof(app_path), "%s", argv[0]);
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    consoleSelect(&bottom);
    LightLock_Init(&hw_lock);
#ifdef APP_RELEASE
    log_init(&bottom, NULL);
    logp("3DSir (built " __DATE__ " " __TIME__ ")\n");
#else
    log_init(&bottom, LOG_PATH);
    logp("--- 3DSir debug (built " __DATE__ " " __TIME__ ") ---\n");
    logp("app: %s\n", app_path);
    net_start();
#endif

    Result rc = iru_open();
    if(R_SUCCEEDED(rc)) {
        logp("default bitrate value: %u\n", cur_bitrate);
        logp("Carrier style: dense (SELECT toggles)\n");
        logp("Repeats: %u (L/R adjust)\n", repeats);
        mkdir(ROOT_DIR, 0777);
        logp("Put Flipper .ir files in\n" ROOT_DIR "/\n");
        browse(ROOT_DIR, true);
    } else {
        logp("ir:u unavailable. START to exit.\n");
        while(running()) {
            gspWaitForVBlank();
            scan_input();
            if(keys_down & KEY_START) break;
        }
    }

#ifndef APP_RELEASE
    net_stop();
#endif
    LightLock_Lock(&hw_lock);
    if(!iru_ok) app_hw_iru(); // a net client left the hardware taken over
    iru_close();
    LightLock_Unlock(&hw_lock);
    logp("--- exit ---\n");
    log_exit();

    free(txbuf);
    free(iru_shm);
    gfxExit();
    if(restart_requested) relaunch_self();
    return 0;
}
