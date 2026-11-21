#pragma once

#include <3ds.h>
#include <stdbool.h>

// Direct access to the IR UART bridge (NXP SC16IS750 on I2C bus 3, address 0x9A), with
// the ir sysmodule stopped. Same approach as Prox3DS does for the NFC controller: kill the
// module, open its i2c::IR / gpio:IR sessions ourselves, relaunch it when done.
//
// Unverified: the I2C device ID (13 is the 3dbrew number for bus 3 / 0x9A) and which
// gpio:IR bits do what. The net server exposes raw I2C, GPIO and IPC ops for that.

#define IR_I2C_DEVID 13

// SC16IS750 registers (datasheet table 10). The I2C sub-address byte is reg << 3 (channel 0).
enum {
    SC_RHR = 0x0, // read; THR on write
    SC_IER = 0x1,
    SC_IIR = 0x2, // read; FCR on write
    SC_LCR = 0x3,
    SC_MCR = 0x4,
    SC_LSR = 0x5,
    SC_MSR = 0x6, // TCR when MCR[2] = 1 and EFR[4] = 1
    SC_SPR = 0x7, // TLR when MCR[2] = 1 and EFR[4] = 1
    SC_TXLVL = 0x8,
    SC_RXLVL = 0x9,
    SC_IODIR = 0xA,
    SC_IOSTATE = 0xB,
    SC_IOINTENA = 0xC,
    SC_IOCONTROL = 0xE,
    SC_EFCR = 0xF,
    // with LCR[7] = 1 (and LCR != 0xBF)
    SC_DLL = 0x0,
    SC_DLH = 0x1,
    // with LCR = 0xBF
    SC_EFR = 0x2,
    SC_XON1 = 0x4,
    SC_XON2 = 0x5,
    SC_XOFF1 = 0x6,
    SC_XOFF2 = 0x7,
};
#define SC_THR SC_RHR
#define SC_FCR SC_IIR

// Stops the ir module (closing ir:u is the caller's job), opens i2c::IR + gpio:IR and
// masks the UART's interrupts (IER = 0), so nothing signals the dead module's IRQ event.
Result irhw_takeover(void);
// Closes our sessions and relaunches the ir module.
void irhw_give_back(void);
bool irhw_owned(void);

Result irhw_i2c_write(u8 dev, const void* buf, u32 size); // I2C cmd 0x14 WriteDeviceRaw
Result irhw_i2c_read(u8 dev, void* buf, u32 size); // I2C cmd 0x15 ReadDeviceRaw, size <= 0x100

// n bytes from / to one register (RHR / THR: n FIFO bytes).
Result sc_read(u8 reg, u8* buf, u32 n);
Result sc_write(u8 reg, const u8* buf, u32 n);
Result sc_get(u8 reg, u8* v);
Result sc_set(u8 reg, u8 v);

// GPIO SetData(value, mask) when mask != 0, then GetData(read_mask).
Result irhw_gpio(u32 value, u32 mask, u32 read_mask, u32* out);

// Any command on the i2c::IR (target 0) or gpio:IR (target 1) session: words[0] is the
// IPC header. out_size > 0 sets up static buffer 0 for a reply of that size.
// reply gets the first 16 command buffer words afterwards.
Result irhw_ipc(int target, const u32* words, u32 n_words, void* out, u32 out_size, u32 reply[16]);

// Logs every readable register (special and enhanced sets included; LCR restored).
void irhw_dump(void);

// Times n single-register reads (LSR) in microseconds.
Result irhw_bench(u32 n, u32* min_us, u32* avg_us, u32* max_us);

u32 irhw_us(u64 ticks);
