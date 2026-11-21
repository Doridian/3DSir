#include "irhw.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

#define MS(x) ((s64)(x) * 1000000LL)

static Handle i2c_handle, gpio_handle;
static u64 ir_tid; // program ID of the ir module we stopped, to relaunch it

// Logs before a call so a hang shows which call never returned.
#define STEP(what, call) ({ logp("  .. %s\n", what); Result _r = (call); logp("  %s: %08lX\n", what, _r); _r; })

u32 irhw_us(u64 ticks) {
    return (u32)(ticks * 1000000ULL / SYSCLOCK_ARM11);
}

// Luma3DS extends svcGetProcessInfo: 0x10000 = process name, 0x10001 = title ID.
static bool find_process(const char* want, u32* pid, u64* tid) {
    u32 pids[64];
    s32 n = 0;
    if(R_FAILED(svcGetProcessList(&n, pids, 64))) return false;
    for(s32 i = 0; i < n; i++) {
        Handle h;
        char name[9] = {0};
        if(R_FAILED(svcOpenProcess(&h, pids[i]))) continue;
        svcGetProcessInfo((s64*)name, h, 0x10000);
        svcGetProcessInfo((s64*)tid, h, 0x10001);
        svcCloseHandle(h);
        if(!strcmp(name, want)) {
            *pid = pids[i];
            return true;
        }
    }
    return false;
}

// srvGetServiceHandle normally blocks while a service's sessions are all in use.
static Result open_service(Handle* out, const char* name) {
    srvSetBlockingPolicy(true);
    Result rc = srvGetServiceHandle(out, name);
    srvSetBlockingPolicy(false);
    return rc;
}

static void close_sessions(void) {
    if(i2c_handle) svcCloseHandle(i2c_handle);
    if(gpio_handle) svcCloseHandle(gpio_handle);
    i2c_handle = gpio_handle = 0;
}

bool irhw_owned(void) {
    return i2c_handle != 0;
}

Result irhw_takeover(void) {
    if(i2c_handle) return 0;
    u32 pid;
    u64 tid;
    if(find_process("ir", &pid, &tid)) {
        logp("ir: pid %lu tid %016llX\n", pid, tid);
        ir_tid = tid;
        if(R_SUCCEEDED(pmAppInit())) {
            STEP("PMAPP_TerminateTitle(ir)", PMAPP_TerminateTitle(tid, MS(3000)));
            pmAppExit();
        }
        u32 pid2;
        if(find_process("ir", &pid2, &tid) && pid2 == pid) {
            Handle h;
            logp("ir still alive, killing pid %lu directly\n", pid);
            if(R_SUCCEEDED(STEP("svcOpenProcess", svcOpenProcess(&h, pid)))) {
                STEP("svcTerminateProcess", svcTerminateProcess(h));
                svcCloseHandle(h);
            }
        }
        svcSleepThread(MS(300));
    } else {
        logp("ir not running\n");
    }

    Result rc_i2c = -1, rc_gpio = -1;
    for(int t = 0; t < 10 && (R_FAILED(rc_i2c) || R_FAILED(rc_gpio)); t++) {
        if(R_FAILED(rc_i2c)) rc_i2c = open_service(&i2c_handle, "i2c::IR");
        if(R_FAILED(rc_gpio)) rc_gpio = open_service(&gpio_handle, "gpio:IR");
        if(R_FAILED(rc_i2c) || R_FAILED(rc_gpio)) svcSleepThread(MS(200));
    }
    logp("open i2c::IR %08lX, gpio:IR %08lX\n", rc_i2c, rc_gpio);
    if(R_FAILED(rc_gpio)) gpio_handle = 0; // optional; raw GPIO ops fail without it
    if(R_FAILED(rc_i2c)) {
        i2c_handle = 0;
        close_sessions();
        return rc_i2c;
    }
    // No UART interrupts while nobody listens for them.
    STEP("IER = 0", sc_set(SC_IER, 0));
    return 0;
}

void irhw_give_back(void) {
    close_sessions();
    if(!ir_tid) return;
    u32 pid;
    u64 tid;
    if(find_process("ir", &pid, &tid)) return;
    FS_ProgramInfo pi = {.programId = ir_tid, .mediaType = MEDIATYPE_NAND};
    if(R_SUCCEEDED(pmAppInit())) {
        Result rc = PMAPP_LaunchTitle(&pi, 0);
        logp("relaunch ir %016llX: %08lX\n", ir_tid, rc);
        pmAppExit();
    }
}

Result irhw_i2c_write(u8 dev, const void* buf, u32 size) {
    if(!i2c_handle) return -1;
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x14, 2, 2);
    cmdbuf[1] = dev;
    cmdbuf[2] = size;
    cmdbuf[3] = IPC_Desc_StaticBuffer(size, 1);
    cmdbuf[4] = (u32)buf;
    Result rc = svcSendSyncRequest(i2c_handle);
    return R_FAILED(rc) ? rc : (Result)cmdbuf[1];
}

Result irhw_i2c_read(u8 dev, void* buf, u32 size) {
    if(!i2c_handle) return -1;
    u32* cmdbuf = getThreadCommandBuffer();
    u32* staticbufs = getThreadStaticBuffers();
    u32 saved[2] = {staticbufs[0], staticbufs[1]};
    cmdbuf[0] = IPC_MakeHeader(0x15, 2, 0);
    cmdbuf[1] = dev;
    cmdbuf[2] = size;
    staticbufs[0] = IPC_Desc_StaticBuffer(size, 0);
    staticbufs[1] = (u32)buf;
    Result rc = svcSendSyncRequest(i2c_handle);
    staticbufs[0] = saved[0];
    staticbufs[1] = saved[1];
    return R_FAILED(rc) ? rc : (Result)cmdbuf[1];
}

// A sub-address write, then a separate read: whether the bridge keeps the sub-address
// across the STOP between the two is one of the things this build is meant to find out.
Result sc_read(u8 reg, u8* buf, u32 n) {
    u8 sub = reg << 3;
    Result rc = irhw_i2c_write(IR_I2C_DEVID, &sub, 1);
    if(R_FAILED(rc)) return rc;
    while(n) {
        u32 k = n > 0x100 ? 0x100 : n;
        rc = irhw_i2c_read(IR_I2C_DEVID, buf, k);
        if(R_FAILED(rc)) return rc;
        buf += k;
        n -= k;
    }
    return 0;
}

Result sc_write(u8 reg, const u8* buf, u32 n) {
    u8 pkt[1 + 64];
    do {
        u32 k = n > 64 ? 64 : n;
        pkt[0] = reg << 3;
        memcpy(pkt + 1, buf, k);
        Result rc = irhw_i2c_write(IR_I2C_DEVID, pkt, 1 + k);
        if(R_FAILED(rc)) return rc;
        buf += k;
        n -= k;
    } while(n);
    return 0;
}

Result sc_get(u8 reg, u8* v) {
    return sc_read(reg, v, 1);
}

Result sc_set(u8 reg, u8 v) {
    return sc_write(reg, &v, 1);
}

// GPIO commands as Prox3DS uses them on gpio:NFC: 0x7 GetData(mask), 0x8 SetData(value, mask).
Result irhw_gpio(u32 value, u32 mask, u32 read_mask, u32* out) {
    *out = 0;
    if(!gpio_handle) return -1;
    u32* cmdbuf = getThreadCommandBuffer();
    if(mask) {
        cmdbuf[0] = IPC_MakeHeader(0x8, 2, 0);
        cmdbuf[1] = value;
        cmdbuf[2] = mask;
        Result rc = svcSendSyncRequest(gpio_handle);
        if(R_SUCCEEDED(rc)) rc = cmdbuf[1];
        if(R_FAILED(rc)) return rc;
    }
    if(!read_mask) return 0;
    cmdbuf[0] = IPC_MakeHeader(0x7, 1, 0);
    cmdbuf[1] = read_mask;
    Result rc = svcSendSyncRequest(gpio_handle);
    if(R_FAILED(rc)) return rc;
    *out = cmdbuf[2];
    return (Result)cmdbuf[1];
}

Result irhw_ipc(int target, const u32* words, u32 n_words, void* out, u32 out_size, u32 reply[16]) {
    Handle h = target ? gpio_handle : i2c_handle;
    if(!h || !n_words || n_words > 16) return -1;
    u32* cmdbuf = getThreadCommandBuffer();
    u32* staticbufs = getThreadStaticBuffers();
    u32 saved[2] = {staticbufs[0], staticbufs[1]};
    memcpy(cmdbuf, words, n_words * 4);
    if(out_size) {
        staticbufs[0] = IPC_Desc_StaticBuffer(out_size, 0);
        staticbufs[1] = (u32)out;
    }
    Result rc = svcSendSyncRequest(h);
    staticbufs[0] = saved[0];
    staticbufs[1] = saved[1];
    memcpy(reply, cmdbuf, 16 * 4);
    return rc;
}

void irhw_dump(void) {
    static const struct {
        u8 reg;
        const char* name;
    } general[] = {
        {SC_IER, "IER"},     {SC_IIR, "IIR"},         {SC_LCR, "LCR"},         {SC_MCR, "MCR"},
        {SC_LSR, "LSR"},     {SC_MSR, "MSR/TCR"},     {SC_SPR, "SPR/TLR"},     {SC_TXLVL, "TXLVL"},
        {SC_RXLVL, "RXLVL"}, {SC_IODIR, "IODir"},     {SC_IOSTATE, "IOState"}, {SC_IOINTENA, "IOIntEna"},
        {SC_IOCONTROL, "IOCtl"}, {SC_EFCR, "EFCR"},
    };
    logp("SC16IS750 registers:\n");
    u8 lcr = 0;
    Result rc = sc_get(SC_LCR, &lcr);
    if(R_FAILED(rc)) {
        logp("  LCR read failed: %08lX\n", rc);
        return;
    }
    for(size_t i = 0; i < sizeof(general) / sizeof(general[0]); i++) {
        u8 v = 0;
        rc = sc_get(general[i].reg, &v);
        if(R_FAILED(rc))
            logp("  %-8s %08lX\n", general[i].name, rc);
        else
            logp("  %-8s %02X%s", general[i].name, v, i % 2 ? "\n" : "   ");
    }

    u8 dll = 0, dlh = 0, efr = 0;
    sc_set(SC_LCR, lcr == 0xBF ? 0x83 : (lcr | 0x80));
    sc_get(SC_DLL, &dll);
    sc_get(SC_DLH, &dlh);
    sc_set(SC_LCR, 0xBF);
    sc_get(SC_EFR, &efr);
    u8 x[4] = {0};
    for(int i = 0; i < 4; i++) sc_get(SC_XON1 + i, &x[i]);
    sc_set(SC_LCR, lcr);
    u32 div = dll | (dlh << 8);
    logp("  DLL/DLH  %04lX (div %lu)  EFR %02X\n", div, div, efr);
    logp("  XON %02X %02X XOFF %02X %02X\n", x[0], x[1], x[2], x[3]);
    if(div) logp("  baud 18.432MHz/16/div = %lu\n", 1152000 / div);
}

Result irhw_bench(u32 n, u32* min_us, u32* avg_us, u32* max_us) {
    u32 lo = ~0u, hi = 0;
    u64 total = 0;
    for(u32 i = 0; i < n; i++) {
        u8 v;
        u64 t0 = svcGetSystemTick();
        Result rc = sc_get(SC_LSR, &v);
        u32 us = irhw_us(svcGetSystemTick() - t0);
        if(R_FAILED(rc)) return rc;
        total += us;
        if(us < lo) lo = us;
        if(us > hi) hi = us;
    }
    *min_us = n ? lo : 0;
    *max_us = hi;
    *avg_us = n ? (u32)(total / n) : 0;
    return 0;
}
