// The IR hardware: sending through ir:u, taking the SC16IS750 over, and learning (see app.h).

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "irhw.h"
#include "log.h"

#define IRU_MAX_TRANSFER 0x800

LightLock hw_lock;
volatile bool trim_overridden;

static u8 cur_bitrate;
static u8* txbuf;
static size_t txbuf_cap;
static Handle send_event;
static u32* iru_shm;
static bool iru_ok; // ir:u open (false while the hardware is taken over)

// ---------------------------------------------------------------- ir:u

bool iru_is_open(void) {
    return iru_ok;
}

Result iru_open(void) {
    if(iru_ok) return 0;
    if(!iru_shm) iru_shm = memalign(0x1000, 0x1000);
    Result rc = -1;
    // Right after a relaunch the module may not have registered ir:u yet. Non-blocking: a dead ir
    // module's ir:u port would otherwise block here forever, holding hw_lock.
    srvSetBlockingPolicy(true);
    for(int t = 0; t < 20; t++) {
        rc = iruInit(iru_shm, 0x1000);
        if(R_SUCCEEDED(rc)) break;
        svcSleepThread(100000000LL);
    }
    srvSetBlockingPolicy(false);
    logp("iruInit: %08lX\n", rc);
    if(R_FAILED(rc)) return rc;
    IRU_GetBitRate(&cur_bitrate);
    rc = IRU_GetSendFinishedEvent(&send_event);
    if(R_FAILED(rc)) logp("GetSendFinishedEvent: %08lX\n", rc);
    iru_ok = true;
    return 0;
}

void iru_close(void) {
    if(!iru_ok) return;
    svcCloseHandle(send_event);
    send_event = 0;
    iruExit();
    iru_ok = false;
    free(txbuf);
    txbuf = NULL;
    txbuf_cap = 0;
}

Result app_hw_direct(u32 flags) {
    iru_close();
    Result rc = irhw_takeover(flags);
    // While the ir module is gone the HOME menu would hang waiting for it.
    aptSetHomeAllowed(false);
    aptSetSleepAllowed(false);
    return rc;
}

void app_hw_iru(void) {
    // Also when the takeover failed halfway: the module may be stopped already.
    if(!irhw_give_back()) {
        // The killed ir module is stuck half-dead: ir:u calls would hang, HOME would hang.
        logp("the ir module can't come back without a reboot\n");
        irhw_reboot();
    }
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

static u32 ms_since(u64 t0) {
    return (svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11;
}

// Sends one transfer and blocks until it has gone out on air. This must not use IRU_WaitSendTransfer
// (or iruSendData with wait) up front: that command *cancels* the transfer in progress, which cut every
// signal off after ~10 ms.
static Result send_chunk(const u8* data, size_t len, u32 baud) {
    u8 state = 0;
    iru_get_transfer_state(&state);
    if(state == 2) IRU_WaitSendTransfer(); // a previous transfer never settled

    svcClearEvent(send_event);
    u64 t0 = svcGetSystemTick();
    Result rc = IRU_StartSendTransfer((u8*)data, len);
    if(R_FAILED(rc)) return rc;

    u32 limit_ms = (u64)len * 10 * 1000 / baud + 200;
    svcWaitSynchronization(send_event, (s64)limit_ms * 1000000LL);
    // Don't trust the event alone; 1 = ready, 2 = sending. The UART cannot be slower than its bit rate,
    // so once the limit has passed all data is out and the transfer is only stuck in the sysmodule's
    // bookkeeping (115200 and 96000 baud do that): cancel it.
    while(R_SUCCEEDED(iru_get_transfer_state(&state)) && state == 2) {
        if(ms_since(t0) > limit_ms) {
            logp("transfer stuck, cancelled\n");
            IRU_WaitSendTransfer();
            break;
        }
        svcSleepThread(1000000LL);
    }
    return 0;
}

// Splits a long byte stream into ir:u sized transfers. Each split adds ~10 ms of silence, so cut in the
// middle of the longest space (run of space_byte) where the extra delay only stretches a gap the
// receiver already ignores.
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

Result app_send_bytes(const IrCarrier* c, const u8* data, size_t n) {
    if(!iru_ok) return -2;
    Result rc = set_carrier(c);
    if(R_FAILED(rc)) return rc;
    return send_bytes(data, n, c->baud, ir_space_byte(c));
}

Result app_send(const IrTimings* tm, IrCarrierStyle st, u32* bytes) {
    if(!iru_ok) return -2; // the hardware is taken over
    const IrCarrier* c = ir_pick_carrier(tm->freq, st);
    Result rc = set_carrier(c);
    if(R_FAILED(rc)) return rc;

    // The net server's trim command overrides this; otherwise the clean style's marks get 80 us shorter.
    // Measured with a Flipper Zero at ~10 cm: they arrive ~80 us long, and with the trim NEC, NECext,
    // NEC42, Samsung32, RC5, RC6, SIRC and Pioneer all decode. The dense style isn't trimmed: its stretch
    // there comes from lone space pulses that only a close, sensitive receiver hears, and it's the one
    // tested at TV distance.
    int32_t saved_trim = ir_mark_trim_us;
    if(!trim_overridden) ir_mark_trim_us = st == IrCarrierClean ? 80 : 0;
    size_t n = ir_render_uart(tm, c, NULL, 0);
    if(n > txbuf_cap) {
        free(txbuf);
        txbuf_cap = n;
        txbuf = malloc(n);
    }
    ir_render_uart(tm, c, txbuf, n);
    ir_mark_trim_us = saved_trim;
    if(bytes) *bytes = n;
    return send_bytes(txbuf, n, c->baud, ir_space_byte(c));
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

Result app_set_led(bool on) {
    return iru_ok ? IRU_SetIRLEDState(on) : -2;
}

// Sends a fixed amount of data at every bit rate and times it. If ir:u keeps the UART busy the whole time,
// the measured rate matches the nominal one; a lower rate means gaps between bytes (or that SetBitRate
// did not take).
void app_timing_check(volatile bool* cancel) {
    static u8 data[IRU_MAX_TRANSFER];
    if(!iru_ok) {
        logp("ir:u is closed (hardware taken over)\n");
        return;
    }
    memset(data, 0xFF, sizeof(data));
    logp("val nominal  got   ms  measured\n");
    for(u8 v = 3; v <= 18 && !*cancel; v++) {
        size_t n = iru_nominal[v] >= 36000 ? sizeof(data) : 256;
        Result rc = IRU_SetBitRate(v);
        u8 got = 0;
        IRU_GetBitRate(&got);
        u64 t0 = svcGetSystemTick();
        if(R_SUCCEEDED(rc)) rc = send_chunk(data, n, iru_nominal[v]);
        u32 us = irhw_us(svcGetSystemTick() - t0);
        if(R_FAILED(rc)) logp("%3u %7lu %4u  err %08lX\n", v, iru_nominal[v], got, rc);
        else logp("%3u %7lu %4u %4lu %8llu\n", v, iru_nominal[v], got, us / 1000, (u64)n * 10 * 1000000 / (us ? us : 1));
    }
    cur_bitrate = 0; // force a SetBitRate on the next send
}

// ---------------------------------------------------------------- learning

size_t app_learn_capture(u32* t, size_t max, volatile bool* cancel) {
    // 8N1 at 38400 baud (1152000 / 30), IrDA. The takeover enabled the transceiver, TX/RX and FIFOs.
    u8 efr = 0, mcr = 0;
    sc_set(SC_LCR, 0xBF);
    sc_get(SC_EFR, &efr);
    sc_set(SC_EFR, efr | 0x10); // MCR[7:5] writable
    sc_set(SC_LCR, 0x83);
    sc_set(SC_DLL, 30);
    sc_set(SC_DLH, 0);
    sc_set(SC_LCR, 0x03);
    sc_get(SC_MCR, &mcr);
    sc_set(SC_MCR, (mcr & 0x3F) | 0x40); // IrDA, clock prescaler 1
    Result rc = sc_set(SC_FCR, 0x07); // FIFOs on, both flushed
    if(R_FAILED(rc)) {
        logp("learn: UART setup failed: %08lX\n", rc);
        return 0;
    }
    // Each burst of carrier is one received byte: only RXLVL is polled (an I2C round trip, ~290 us), and
    // a byte's time is the middle of the poll that first saw it. The FIFO is flushed when half full.
    size_t n = 0;
    u8 prev = 0;
    u64 last_poll = svcGetSystemTick(), first = 0, last_edge = 0;
    const u64 quiet = SYSCLOCK_ARM11 / 4; // 250 ms after the last burst
    while(!*cancel && n < max) {
        u8 lvl = 0;
        rc = sc_get(SC_RXLVL, &lvl);
        u64 now = svcGetSystemTick();
        if(R_FAILED(rc)) {
            logp("learn: RXLVL read failed: %08lX\n", rc);
            break;
        }
        if(lvl > prev) {
            u64 at = (now + last_poll) / 2;
            if(!n) first = at;
            for(u8 k = prev; k < lvl && n < max; k++) t[n++] = irhw_us(at - first);
            last_edge = now;
        }
        prev = lvl;
        // A byte that arrives during the flush is lost: flush between frames (3 ms quiet is longer than any
        // bit), or when the 64-byte FIFO is about to overflow.
        if((lvl >= 8 && now - last_edge > SYSCLOCK_ARM11 / 333) || lvl >= 56) {
            sc_set(SC_FCR, 0x03); // RX FIFO flushed, FIFOs stay on
            prev = 0;
        }
        last_poll = now;
        if(n && now - last_edge > quiet) break;
    }
    return n;
}
