# 3DSir

Turns the 3DS infrared port into a universal TV remote that reads, edits and learns Flipper Zero `.ir`
files.

## Status

It works. With the dense carrier (the default), an LG OLED TV (built-in NEC
codes) and a Denon amp (Flipper raw captures) respond reliably. A Flipper
Zero capture of the 3DS sending a Denon code at 10–20 cm decodes bit for bit.

The clean carrier also works, but only with careful aiming. Its pulses are
half as long, which gives about 6 dB less signal at 38 kHz. Range is limited
by the IrDA LED either way.

With a Flipper Zero right next to the port (`tools/flipper.py`), the clean
carrier decodes for NEC, NECext, NEC42, Samsung32, RC5, RC6, SIRC/15/20 and
Pioneer, once its marks are trimmed by 80 µs (they arrive ~80 µs long, the
spaces as much short). At that range the dense carrier doesn't decode: the lone
pulses in its spaces stretch marks by ~175 µs, ±90 µs depending on where an edge
falls in its UART frame. So dense stays the default for TVs and clean gets the
trim. RCA (56 kHz) can't be checked with a Flipper's 38 kHz receiver.

Every press sends the signal N times (L/R, default 3), using a raw capture's
trailing gap between copies. Denon receivers ignore a single frame.

Learning works for pulse-distance codes: NEC, NECext, NEC42(ext), Samsung32,
SIRC/15/20, Kaseikyo, Pioneer and RCA, tested with a Flipper Zero sending at
the port. RC5 and RC6 can't be learned (see "Learning" below).

## The hardware

- The IR port is an NXP **SC16IS750** I2C-to-UART bridge (i2c::IR, bus 3, address
  0x9A) driving an IrDA transceiver. It speaks **IrDA SIR** only. It cannot
  output a raw 38 kHz carrier, and it cannot capture one.
- The UART clock is 18.432 MHz / 16 = 1152000 Hz. `ir:u`'s `SetBitRate`
  includes **36000** (value 7), **38400** (15), 48000 (6) and 57600 (14) baud.
- In IrDA SIR mode, every `0` bit on the wire becomes a light pulse 3/16 of a
  bit long, and every `1` bit becomes darkness.
- Measured with the chip taken over: the clock is right (divisors 12/24/48 give
  95.5k/47.8k/24.0k bit/s), and I2C keeps the 64-byte TX FIFO from running dry
  even at 96000 baud (a register read takes ~290 µs).
- The bridge's GPIO0 is the transceiver's shutdown pin: high (as the `ir` module
  leaves it when idle) nothing goes out or comes in. When idle the module also
  leaves TX and RX disabled (EFCR = 0x06) and the FIFOs off.
- Receiving works in IrDA mode: each burst of a remote's carrier arrives as one
  `0x00` byte, so the gaps between bytes are the gaps between mark starts (a
  Flipper's NEC: 13.6 ms leader, then 1.1 / 2.2 ms bits, decodable by hand).
  Polling over I2C times them to about ±150 µs; mark widths aren't visible at 38400.

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
signal. This project generates signals from a code database instead.

Timing the received bytes instead of replaying them does learn pulse-distance
codes (NEC, Samsung, ...), where every bit is in the time from one mark's start
to the next. See "Learning" below.

## Learning

Learning takes the SC16IS750 over (as the debug build's diagnostic does, see
below), sets 38400 baud IrDA, and polls RXLVL over I2C (~290 µs a poll). Each
burst of a 38/40 kHz carrier arrives as one byte, so the capture is the list of
times the marks start. Measured with a Flipper Zero:

- Mark starts are late by up to about a poll, a few by ~400 µs: an interval can be
  off by ±350 µs. That's close to the 600 µs between SIRC's 0 and 1, and more than
  half of Kaseikyo's 864 µs bit.
- So `irdecode.c` doesn't classify intervals alone. It predicts each mark's start
  from a reference that absorbs half of every edge's error, votes bits across
  every repeated frame in the capture (SIRC sends three), and checks the
  protocol's own redundancy (inverted bytes, Kaseikyo's parity). The closest
  fitting protocol wins (NEC and Pioneer differ only in timing).
- SIRC and Pioneer have no stop mark, so their last bit's interval runs into the
  next frame and can't be seen. It's taken as 0 (always 0 for Pioneer; SIRC's
  address top bit, usually 0).
- Mark lengths aren't visible, so raw (unknown protocol) signals can't be learned.
- 36 kHz carriers (RC5, RC6) don't arrive as one byte per burst but as a stream of
  bytes every ~600 µs, also across short spaces: no usable edges. 36000 baud is no
  better: there even 40 kHz bursts become streams.

`tests/decode_test.c` holds the captures (Flipper `ir tx` at ~5 cm) and checks every protocol with
jittered timings.

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

Put Flipper Zero `.ir` files in `sdmc:/3ds/3dsir/`. Subfolders work. The
[Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB) doesn't need copying:
**Remote database** downloads it over Wi-Fi.
The Flipper firmware's universal `tv.ir`
(`applications/main/infrared/resources/infrared/assets/tv.ir`) is the most
useful file: select a `Power` entry and press **Y** to send every `Power` code
in turn, TV-B-Gone style.

Everything works with the buttons on the top screen; the bottom screen repeats
them as touch buttons (a remote's buttons, the keyboard, the footer's hints):

- **Remotes**: browse the `.ir` files. **A** opens one, **X** renames or deletes
  it, **Y** makes a new remote (learn its first button, or empty).
- A remote: **A** sends the selected button (hold to keep sending), **Y** sends
  every button with the same name, **L/R** set the repeats per press, **X** edits:
  learn a new button, re-learn this one, rename, move, delete. Changes are saved
  to the file right away, in the Flipper's format (comments in it aren't kept).
- **Find my remote**: recognizes your remote from a few button presses, with all its
  buttons, so you don't have to learn them one by one. See "Finding a remote" below.
- **Remote database**: downloads or updates the Flipper-IRDB over Wi-Fi, or indexes the
  remotes on the SD card again.
- **Learn new remote**: point a remote at the IR port on the back of the 3DS
  and press a button. Then name the button and the remote, and it is saved as
  `sdmc:/3ds/3dsir/<name>.ir`.
- **Settings**: carrier style and repeats, kept in `sdmc:/3ds/3dsir/.settings`.
- **SELECT** shows the log, **START** exits.

Supported protocols: NEC, NECext, NEC42, NEC42ext, Samsung32, SIRC, SIRC15,
SIRC20, RC5, RC5X, RC6, Kaseikyo, Pioneer, RCA, and raw.

### Finding a remote

**Find my remote** asks you to press Power on your remote (pointed at the IR port), then
whichever button best tells the remaining candidates apart: Vol+, Mute, a digit, and so on.
**X** skips a button your remote doesn't have, and **Y** shows the matches so far. A match
opens like any remote, so you can try its buttons. **X** there saves a copy to the top of
Remotes.

- It searches every `.ir` file in `sdmc:/3ds/3dsir`, and the Flipper-IRDB once
  **Remote database** has downloaded it. The download is the newest commit's tarball from GitHub
  (about 18 MB). It uses libcurl and mbedtls, with the CA bundle built into the app
  (`data/cacert.pem`, Mozilla's, via curl.se). The ~9000 `.ir` files are packed into one file,
  `sdmc:/3ds/3dsir/Flipper-IRDB.pack` (~65 MB), and indexed on the way in. That takes about
  4 minutes on top of the download. The same files as a folder tree took over 25 minutes: on the
  3DS's SD card each new folder costs ~240 ms and each small file ~60 ms. A finished download
  is kept until it's unpacked, so a retry doesn't fetch it again. An update replaces the pack only
  once the new one is complete.
- **Remotes > Flipper-IRDB** browses the pack like a folder. Its remotes open read-only:
  **X** saves an editable copy to Remotes. A real folder of that name in `sdmc:/3ds/3dsir`
  (a copy put there by hand) is hidden while the pack exists. Without the pack, it's
  searched like any other remotes.
- The index is kept in `sdmc:/3ds/3dsir/.irdb`, and can always be rebuilt from the pack and
  the files. Remotes the app saves, renames or deletes are indexed again automatically. After
  changing `.ir` files on a PC, use **Remote database > Y**.
- A code's key is what the learning decoder makes of it. Each database signal, parsed or
  raw, is encoded, cut down to mark starts and decoded, the same way a capture is. So
  `NECext 04FB/08F7` in one file, `NEC 04/08` in another, and a raw capture of either all
  match one press. Only codes the 3DS can learn can be matched: 7035 of the Flipper-IRDB's
  8901 files have at least one. RC5/RC6 remotes and air conditioners' long frames can't be.
- The next button to ask is the one that leaves the fewest candidates on average. It is
  only asked if that's at least 15% fewer. Past that point, what's left is mostly the same
  remote filed more than once. When you're asked for a named button, remotes whose button
  of that name sends a different code are dropped. Skipping a button drops the remotes that
  have it (a remote without Down isn't then asked for Up). Neither filter is applied if it
  would leave no remotes. Simulated on the whole Flipper-IRDB (`tests/irdb_test.c`, every press
  decoded from jittered mark starts), every remote is found, after 2.75 presses on average. 97.5%
  end with only remotes that send the same codes for every button you could press.

### Suggested first test

1. **Tools > Carrier test**, hold A, and look at the IR port (on the back of the
   N3DS XL) through a phone camera. Front cameras usually have no IR filter.
2. **Built-in test codes**: hold the 3DS about 30 cm from the TV and send
   the Vol+ code for its brand.

## Debug build

The debug build adds:

- **Tools > IR chip diagnostic**: closes `ir:u` and talks to the SC16IS750 directly over the `ir`
  module's own `i2c::IR` session (see below). It logs every register (the
  divisor, IrDA mode, the bridge's own GPIO pins) and the I2C round-trip time. Then it counts
  what the RX FIFO receives, so you can point a remote at the port. B puts the chip back the
  way the module left it and reopens `ir:u`.
- A log in `sdmc:/3dsir.log`.
- A TCP server on port 8020 while Wi-Fi is up. The address is shown on the top screen.
  `tools/irnet.py HOST ...` uses it to:
  - update the app in place (`update`; it relaunches itself through Rosalina's `hb:ldr`),
    copy `.ir` files onto the SD card (`put`), grab either screen (`shot [bottom]`), press
    buttons (`keys`), tap the touchscreen (`touch X Y`) and read the log (`log`);
  - send codes through `ir:u` from a `.ir` file on the PC (`send FILE NAME`) or by
    protocol (`code NEC 0x04 0x08`), or raw UART bytes (`irusend`);
  - take the SC16IS750 over (`takeover` / `release`), read and write its registers (`regs`,
    `rd`, `wr`), set any baud divisor with IrDA on or off (`uart`), push bytes through the
    TX FIFO (`tx`, `txfill`), record what the RX FIFO receives with timestamps (`rx`),
    time I2C (`bench`), and run raw GPIO, I2C and IPC commands (`gpio`, `i2c`, `ipc`).

How the takeover works:

- `i2c::IR` allows one session, and the `ir` module holds it. Luma3DS's
  `svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, ...)` (SVC 0xB0) gives us a second
  handle to that same session. With `ir:u` closed the module is idle, and the takeover masks
  the chip's interrupts so it stays that way.
- The takeover saves the registers the module left. When idle it keeps the chip in sleep mode
  (IER = 0x10), the transceiver shut down (bridge GPIO0, its SD pin, high), TX and RX
  disabled (EFCR = 0x06) and the FIFOs off. Then it turns all of that on. Giving back
  restores them, closes our handle (the module's stays open) and reopens `ir:u`. No reboot.
- The old way, `takeover kill`, still exists. It doesn't work well: `ir` ignores PM's
  terminate, `svcTerminateProcess` leaves it half dead (still listed, its `ir:u` hangs), and
  relaunching it is impossible, so giving back then reboots the console.
- Don't open `gpio:IR` (`takeover gpio`) after a kill: when that session closes, `gpio`
  crashes with 0xD8E007F7. `gpio:IR` GetData (command 7, as on `gpio:NFC`) fails anyway
  (0xE0E033EA).

Verified: I2C device 13 is the bridge, and a register read is a sub-address write followed
by a separate read.

`tools/flipper.py` drives a Flipper Zero over USB as the other end: `rx SECONDS [raw]` prints
what it receives (with `tools/irnet.py HOST code ...` from another shell), `tx PROTOCOL ADDR
CMD` sends at the 3DS for `irnet.py rx`.

Example session:

```
tools/irnet.py 192.168.1.50 takeover           # shares ir's session; transceiver, TX/RX on
tools/irnet.py 192.168.1.50 regs
tools/irnet.py 192.168.1.50 uart 30 irda       # 38400 baud
tools/irnet.py 192.168.1.50 txfill 0x00 4000   # ~1 s of 38.4 kHz carrier
tools/irnet.py 192.168.1.50 rx 3000            # press a remote button at the port
tools/irnet.py 192.168.1.50 release            # chip back as it was, ir:u reopened
```

## Host test

```
cc -O2 -Isource tests/host_test.c source/irenc.c source/irfile.c -o host_test
./host_test tv.ir
```

The test renders every signal, simulates the IrDA encoder and a receiver, and
checks the demodulated timings. It also checks known on-air codes: LG power
`0x20DF10EF`, Samsung power `0xE0E040BF`, and Sony power `0xA90`.

```
cc -O2 -Isource tests/decode_test.c source/irdecode.c source/irenc.c -o decode_test
./decode_test
```

The learning decoder: real captures, and every protocol encoded, reduced to mark starts and
jittered.

```
cc -O2 -Isource tests/irdb_test.c source/irdb.c source/irdecode.c source/irenc.c source/irfile.c -o irdb_test
./irdb_test [path/to/Flipper-IRDB]
```

The remote index and the finder: a few made-up remotes. Given a Flipper-IRDB checkout, it also
finds every detectable remote in it by simulated presses, and prints how many presses that took.

## License

AGPL-3.0, see `LICENSE`. The protocol encodings follow the Flipper Zero
firmware's `lib/infrared` (GPL-3.0).
`data/cacert.pem` is Mozilla's CA certificate list (MPL-2.0), as extracted by curl.se.
The Flipper-IRDB isn't part of this project: the app downloads it from GitHub when asked.
