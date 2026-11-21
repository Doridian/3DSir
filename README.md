# 3DSir

An experiment to turn the 3DS infrared port into a universal TV remote.

## Status

It works. With the dense carrier (the default), an LG OLED TV (built-in NEC
codes) and a Denon amp (Flipper raw captures) respond reliably. A Flipper
Zero capture of the 3DS sending a Denon code at 10–20 cm decodes bit for bit.

The clean carrier also works, but only with careful aiming. Its pulses are
half as long, which gives about 6 dB less signal at 38 kHz. Range is limited
by the IrDA LED either way.

Every press sends the signal N times (L/R, default 3), using a raw capture's
trailing gap between copies. Denon receivers ignore a single frame.

## The hardware

- The IR port is an NXP **SC16IS750** I2C-to-UART bridge (i2c::IR, bus 3, address
  0x9A) driving an IrDA transceiver. It speaks **IrDA SIR** only. It cannot
  output a raw 38 kHz carrier, and it cannot capture one.
- The UART clock is 18.432 MHz / 16 = 1152000 Hz. `ir:u`'s `SetBitRate`
  includes **36000** (value 7), **38400** (15), 48000 (6) and 57600 (14) baud.
- In IrDA SIR mode, every `0` bit on the wire becomes a light pulse 3/16 of a
  bit long, and every `1` bit becomes darkness.

## The trick

Run the UART at the carrier frequency and treat each bit as one carrier cycle:

| byte | on air (38400 baud) |
|------|---------------------|
| `0x00` | start + 8 data bits: 9 pulses at 38.4 kHz, then 1 missing pulse (stop bit) |
| `0xFF` | a single start-bit pulse, then darkness for 9 bit times |

- **Marks:** a run of `0x00` bytes is a 38.4 kHz carrier at about 19% duty
  cycle, missing every tenth pulse.
- **Spaces:** a run of `0xFF` bytes leaves only one lone pulse per 260 µs.
  These pulses still repeat at an exact subharmonic of the carrier (3.84 kHz ×
  10 = 38.4 kHz), so the gaps carry a weak carrier about 18 dB below the marks.
  Receivers ignore it at normal distances, but a very close, sensitive receiver
  can hear it.

A mark/space waveform is sampled at 1/baud (26 µs) and packed into UART
frames. The resulting byte stream is sent with `ir:u` `StartSendTransfer`
(raw bytes, up to 0x800 per transfer). Edges land within about 2 carrier
cycles of the intended time. That is less than the ±5 cycles of output
distortion a TSOP-style receiver adds by its own datasheet.

A second "clean" carrier style (SELECT toggles) sends `0x55` at twice the
carrier rate: 72000/96000/115200 baud give 36/48/57.6 kHz. Pulses land on
every even bit, which keeps the start and stop bits in phase, so this carrier
has no missing pulses. Its spaces use `0xE6`, whose odd-slot pulses are in
antiphase with the start bit and cancel its carrier component. The trade-off
is a lower duty cycle (~9%, about 6 dB weaker) and no exact 38 kHz option.

The 115200 and 96000 baud rates sometimes stay in the "sending" state forever
on real hardware. `send_chunk` detects this and cancels the transfer.

**[Bit rate / timing check]** times a fixed-size send at every bit rate. The
measured rate should match the nominal one. If it's lower, either `ir:u`
leaves gaps between bytes or `SetBitRate` didn't take effect.

Carrier choice in the dense style: 36 kHz protocols (RC5/RC6) use 36000 baud. 38 and 40 kHz
protocols (NEC, Samsung, Sony, Kaseikyo) use 38400. 56 kHz (RCA) uses 57600.

## Why earlier attempts failed

[3DS-Remote](https://github.com/RedTopper/3DS-Remote) recorded the bytes the
UART received from a real remote and replayed them. The IrDA decoder throws
away the carrier and the space timings, so a replay can't reproduce the
signal. Learning codes from a remote is not realistically possible. This
project generates signals from a code database instead.

## Pitfall: `IRU:WaitSendTransfer` cancels

`WaitSendTransfer` *cancels* the transfer in progress and then waits for the
idle state, so libctru's `iruSendData(..., wait=true)` cuts every
transmission off after about 10 ms. Wait for `GetSendFinishedEvent` and poll
`GetTransferState` (0x000F0000) until it reports 1 (ready) instead.

## Hardware findings

1. `ir:u` keeps the UART FIFO fed: transfer times match the nominal bit rate
   up to 72000 baud, with about 7 ms of fixed overhead.
2. Range is short and aiming matters. The LED is an IrDA part.
3. `SetIRLEDState` isn't needed to transmit. Enabling it gives one brief flash
   that the hardware ends on its own.

## Download

**[Latest release](https://github.com/Doridian/3DSir/releases/latest)**: get `3DSir.3dsx` or `3DSir.cia`.
CI builds every `v*` tag into a release:

| File | For |
| --- | --- |
| `3DSir.3dsx` | The Homebrew Launcher. |
| `3DSir.cia` | The same build, to install to the HOME menu (e.g. with FBI). |
| `3DSir-debug.3dsx` | Development: adds the IR chip diagnostic, a TCP debug server on port 8020 and a log file on the SD card. |

Builds of every commit are attached to its CI run as artifacts.

## Usage

Build with devkitARM:

```
docker run --rm -v "$PWD":/src:Z -w /src docker.io/devkitpro/devkitarm make
```

`make` is the debug build (`3dsir.3dsx`), `make release` the release build (`3dsir-release.3dsx`).
`make cia` builds the release build as `3dsir-release.cia` (there is no debug CIA); it needs
[makerom](https://github.com/3DSGuy/Project_CTR) and
[bannertool](https://github.com/diasurgical/bannertool) on the `PATH` (or `MAKEROM=` / `BANNERTOOL=`).
The icon, HOME menu banner and jingle are drawn by `meta/make_art.py`.

Copy `3DSir.3dsx` (or a local build's `3dsir.3dsx`) to `sdmc:/3ds/` and launch it from the Homebrew Launcher
(Luma3DS gives it `ir:u` access), or install `3DSir.cia` and launch it from
the HOME menu.

Put Flipper Zero `.ir` files in `sdmc:/3ds/3dsir/`. Subfolders work, so the
whole [Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB) can go there.
The Flipper firmware's universal `tv.ir`
(`applications/main/infrared/resources/infrared/assets/tv.ir`) is the most
useful file: select a `Power` entry and press **Y** to send every `Power` code
in turn, TV-B-Gone style.

Supported protocols: NEC, NECext, NEC42, NEC42ext, Samsung32, SIRC, SIRC15,
SIRC20, RC5, RC5X, RC6, Kaseikyo, Pioneer, RCA, and raw.

### Suggested first test

1. **[Carrier test]**, hold A, and look at the IR port (on the back of the
   N3DS XL) through a phone camera. Front cameras usually have no IR filter.
2. **[Built-in test codes]**: hold the 3DS about 30 cm from the TV and send
   the Vol+ code for its brand.

## Debug build

The debug build adds:

- **[IR chip diagnostic]**: stops the system's `ir` module and talks to the SC16IS750 directly
  over `i2c::IR`, the way Prox3DS takes over the NFC controller. It logs every register (the
  divisor, IrDA mode, the bridge's own GPIO pins) and the I2C round-trip time. Then it counts
  what the RX FIFO receives, so you can point a remote at the port. B gives the hardware back
  and relaunches the `ir` module. ZL/ZR and the C-stick (served by the same module) don't work
  while it's taken over.
- A log in `sdmc:/3dsir.log`.
- A TCP server on port 8020 while Wi-Fi is up. The address is shown on the top screen.
  `tools/irnet.py HOST ...` uses it to:
  - update the app in place (`update`; it relaunches itself through Rosalina's `hb:ldr`),
    copy `.ir` files onto the SD card (`put`), grab the top screen (`shot`), press
    buttons (`keys`) and read the log (`log`);
  - send codes through `ir:u` from a `.ir` file on the PC (`send FILE NAME`) or by
    protocol (`code NEC 0x04 0x08`), or raw UART bytes (`irusend`);
  - take the SC16IS750 over (`takeover` / `release`), read and write its registers (`regs`,
    `rd`, `wr`), set any baud divisor with IrDA on or off (`uart`), push bytes through the
    TX FIFO (`tx`, `txfill`), record what the RX FIFO receives with timestamps (`rx`),
    time I2C (`bench`), and run raw GPIO, I2C and IPC commands (`gpio`, `i2c`, `ipc`).

Still unverified on hardware: the I2C device ID (13, the 3dbrew number for bus 3 /
0x9A), whether a register read works as a sub-address write followed by a separate read,
and which `gpio:IR` bits do what.

Example session:

```
tools/irnet.py 192.168.1.50 takeover
tools/irnet.py 192.168.1.50 regs
tools/irnet.py 192.168.1.50 uart 15 nrz        # 76800 baud, IrDA off
tools/irnet.py 192.168.1.50 txfill 0x55 4000   # a 50% duty 38.4 kHz square wave?
tools/irnet.py 192.168.1.50 rx 2000            # press a remote button at the port
tools/irnet.py 192.168.1.50 release
```

## Host test

```
cc -O2 -Isource tests/host_test.c source/irenc.c source/irfile.c -o host_test
./host_test tv.ir
```

The test renders every signal, simulates the IrDA encoder and a receiver, and
checks the demodulated timings. It also checks known on-air codes: LG power
`0x20DF10EF`, Samsung power `0xE0E040BF`, and Sony power `0xA90`.

## License

AGPL-3.0, see `LICENSE`. The protocol encodings follow the Flipper Zero
firmware's `lib/infrared` (GPL-3.0).
