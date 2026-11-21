#include "irhw.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

#define MS(x) ((s64)(x) * 1000000LL)

static Handle i2c_handle, gpio_handle;
static u64 ir_tid; // program ID of the ir module we stopped, to relaunch it
static u32 ir_killed_pid;
static bool stolen; // i2c_handle is a second handle to the ir module's own session

// What the ir module left in the chip, put back on give-back after a steal.
static struct {
    u8 ier, lcr, mcr, iodir, iostate, iointena, ioctl, efcr, dll, dlh, efr;
    bool fifo_on;
} saved;

// Luma3DS's svcControlService (0xB0), SERVICEOP_STEAL_CLIENT_SESSION (0): a new handle to an
// existing client session of a service, here the one the ir module holds.
static Result steal_session(Handle* out, const char* name) {
    register u32 r0 __asm__("r0") = 0;
    register u32 r1 __asm__("r1") = (u32)out;
    register u32 r2 __asm__("r2") = (u32)name;
    __asm__ volatile("svc 0xB0" : "+r"(r0), "+r"(r1), "+r"(r2) : : "r3", "r12", "memory");
    return (Result)r0;
}

static Result save_regs(void) {
    Result rc = 0;
    u8 iir = 0;
#define GET(reg, dst) if(R_SUCCEEDED(rc)) rc = sc_get(reg, &dst)
    GET(SC_IER, saved.ier);
    GET(SC_LCR, saved.lcr);
    GET(SC_MCR, saved.mcr);
    GET(SC_IODIR, saved.iodir);
    GET(SC_IOSTATE, saved.iostate);
    GET(SC_IOINTENA, saved.iointena);
    GET(SC_IOCONTROL, saved.ioctl);
    GET(SC_EFCR, saved.efcr);
    GET(SC_IIR, iir);
    saved.fifo_on = (iir & 0xC0) != 0;
    if(R_SUCCEEDED(rc)) rc = sc_set(SC_LCR, 0x80 | saved.lcr);
    GET(SC_DLL, saved.dll);
    GET(SC_DLH, saved.dlh);
    if(R_SUCCEEDED(rc)) rc = sc_set(SC_LCR, 0xBF);
    GET(SC_EFR, saved.efr);
#undef GET
    sc_set(SC_LCR, saved.lcr);
    logp("saved: IER %02X LCR %02X MCR %02X IODir %02X IOState %02X IOIntEna %02X IOCtl %02X EFCR %02X "
         "div %u EFR %02X FIFOs %s: %08lX\n", saved.ier, saved.lcr, saved.mcr, saved.iodir, saved.iostate,
         saved.iointena, saved.ioctl, saved.efcr, saved.dll | saved.dlh << 8, saved.efr,
         saved.fifo_on ? "on" : "off", rc);
    return rc;
}

static void restore_regs(void) {
    sc_set(SC_IER, 0);
    sc_set(SC_LCR, 0xBF);
    sc_set(SC_EFR, saved.efr | 0x10); // MCR[7:5,2] are only writable with EFR[4]
    sc_set(SC_LCR, 0x80 | saved.lcr);
    sc_set(SC_DLL, saved.dll);
    sc_set(SC_DLH, saved.dlh);
    sc_set(SC_LCR, saved.lcr);
    sc_set(SC_MCR, saved.mcr);
    sc_set(SC_LCR, 0xBF);
    sc_set(SC_EFR, saved.efr);
    sc_set(SC_LCR, saved.lcr);
    sc_set(SC_FCR, 0x06); // flush both FIFOs
    sc_set(SC_FCR, saved.fifo_on ? 0x01 : 0x00);
    sc_set(SC_IODIR, saved.iodir);
    sc_set(SC_IOSTATE, saved.iostate);
    sc_set(SC_IOINTENA, saved.iointena);
    sc_set(SC_IOCONTROL, saved.ioctl & 0x07); // bit 3 is a software reset
    sc_set(SC_EFCR, saved.efcr);
    Result rc = sc_set(SC_IER, saved.ier);
    logp("give back: registers restored: %08lX\n", rc);
}

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

Result irhw_takeover(u32 flags) {
    if(i2c_handle) return 0;
    u32 pid;
    u64 tid;
    logp("takeover: %s, %s\n", flags & IRHW_STEAL ? "steal ir's i2c::IR session" : flags & IRHW_KILL ? "kill ir" : "ir stays",
         flags & IRHW_GPIO ? "gpio:IR" : "no gpio:IR");
    if(flags & IRHW_STEAL) {
        Result rc = STEP("steal i2c::IR", steal_session(&i2c_handle, "i2c::IR"));
        if(R_FAILED(rc)) {
            i2c_handle = 0;
            return rc;
        }
        stolen = true;
        rc = save_regs();
        if(R_FAILED(rc)) {
            svcCloseHandle(i2c_handle);
            i2c_handle = 0;
            stolen = false;
            return rc;
        }
        goto configure;
    }
    if(!(flags & IRHW_KILL)) {
        // nothing to stop
    } else if(find_process("ir", &pid, &tid)) {
        logp("ir: pid %lu tid %016llX\n", pid, tid);
        ir_tid = tid;
        if(R_SUCCEEDED(pmAppInit())) {
            STEP("PMAPP_TerminateTitle(ir)", PMAPP_TerminateTitle(tid, MS(3000)));
            pmAppExit();
        }
        // PM's terminate only asks: ir unbinds its GPIO interrupt and exits on its own time.
        // Killing it before then leaves gpio with a stale binding, and gpio crashes
        // (0xD8E007F7) when the relaunched ir binds again.
        u32 pid2 = 0;
        u64 t0 = svcGetSystemTick();
        bool alive;
        while((alive = find_process("ir", &pid2, &tid) && pid2 == pid) &&
              svcGetSystemTick() - t0 < 5ULL * SYSCLOCK_ARM11)
            svcSleepThread(MS(20));
        logp("ir %s after %lu ms\n", alive ? "still alive" : "exited",
             (u32)((svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11));
        if(alive) {
            Handle h;
            logp("ir still alive, killing pid %lu directly\n", pid);
            ir_killed_pid = pid;
            if(R_SUCCEEDED(STEP("svcOpenProcess", svcOpenProcess(&h, pid)))) {
                STEP("svcTerminateProcess", svcTerminateProcess(h));
                svcCloseHandle(h);
            }
        }
        svcSleepThread(MS(300));
    } else {
        logp("ir not running\n");
    }
    if(!(flags & IRHW_GPIO)) gpio_handle = 0;

    Result rc_i2c = -1, rc_gpio = -1;
    for(int t = 0; t < 10 && (R_FAILED(rc_i2c) || R_FAILED(rc_gpio)); t++) {
        if(R_FAILED(rc_i2c)) rc_i2c = open_service(&i2c_handle, "i2c::IR");
        if(R_FAILED(rc_gpio)) rc_gpio = flags & IRHW_GPIO ? open_service(&gpio_handle, "gpio:IR") : 0;
        if(R_FAILED(rc_i2c) || R_FAILED(rc_gpio)) svcSleepThread(MS(200));
    }
    logp("open i2c::IR %08lX, gpio:IR %08lX\n", rc_i2c, rc_gpio);
    if(R_FAILED(rc_gpio)) gpio_handle = 0; // optional; raw GPIO ops fail without it
    if(R_FAILED(rc_i2c)) {
        i2c_handle = 0;
        close_sessions();
        return rc_i2c;
    }
configure:
    // No UART interrupts while nobody listens for them (after a steal: so the idle ir module
    // doesn't wake up and use the chip under us).
    STEP("IER = 0", sc_set(SC_IER, 0));
    // The ir module leaves the chip idle: transceiver shut down (bridge GPIO0 high, its SD pin),
    // TX and RX disabled (EFCR[2:1]), FIFOs off. Without these nothing goes out or comes in.
    u8 io = 0;
    if(R_SUCCEEDED(sc_get(SC_IOSTATE, &io))) STEP("IOState GPIO0 = 0 (transceiver on)", sc_set(SC_IOSTATE, io & ~1));
    u8 efcr = 0;
    if(R_SUCCEEDED(sc_get(SC_EFCR, &efcr))) STEP("EFCR TX/RX enable", sc_set(SC_EFCR, efcr & ~0x06));
    STEP("FCR FIFOs on + reset", sc_set(SC_FCR, 0x07));
    return 0;
}

bool irhw_give_back(void) {
    if(stolen) {
        restore_regs();
        svcCloseHandle(i2c_handle); // only our handle: the ir module's own stays open
        i2c_handle = 0;
        stolen = false;
        logp("give back: stolen session closed\n");
        return true;
    }
    if(i2c_handle) {
        logp("give back: closing i2c::IR\n");
        svcCloseHandle(i2c_handle);
        i2c_handle = 0;
    }
    if(gpio_handle) {
        logp("give back: closing gpio:IR\n");
        svcCloseHandle(gpio_handle);
        gpio_handle = 0;
        svcSleepThread(MS(200)); // a gpio crash on session cleanup would land here
        logp("give back: gpio:IR closed\n");
    }
    if(!ir_tid) return true;
    u32 pid;
    u64 tid;
    // A killed process stays listed until every handle to it is closed.
    u64 t0 = svcGetSystemTick();
    bool found;
    while((found = find_process("ir", &pid, &tid)) && pid == ir_killed_pid &&
          svcGetSystemTick() - t0 < 5ULL * SYSCLOCK_ARM11)
        svcSleepThread(MS(50));
    if(found) {
        logp("give back: ir pid %lu still listed (killed pid %lu) after %lu ms, not relaunching\n", pid,
             ir_killed_pid, (u32)((svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11));
        return pid != ir_killed_pid; // a different pid: somebody relaunched it
    }
    logp("give back: old ir gone after %lu ms\n", (u32)((svcGetSystemTick() - t0) * 1000 / SYSCLOCK_ARM11));
    FS_ProgramInfo pi = {.programId = ir_tid, .mediaType = MEDIATYPE_NAND};
    if(R_SUCCEEDED(pmAppInit())) {
        logp("give back: launching ir %016llX\n", ir_tid);
        Result rc = PMAPP_LaunchTitle(&pi, 0);
        logp("relaunch ir %016llX: %08lX\n", ir_tid, rc);
        pmAppExit();
        if(R_FAILED(rc)) return false;
    }
    ir_tid = 0;
    ir_killed_pid = 0;
    return true;
}

void irhw_reboot(void) {
    logp("rebooting\n");
    APT_HardwareResetAsync();
    svcSleepThread(MS(3000));
    logp("APT reset didn't happen, svcKernelSetState(7)\n");
    svcKernelSetState(7);
    for(;;) svcSleepThread(MS(1000));
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
