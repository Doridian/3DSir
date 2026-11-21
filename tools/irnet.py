#!/usr/bin/env python3
"""PC side of the 3DSir debug build's network server (source/netsrv.c, TCP port 8020).

usage: irnet.py HOST COMMAND [ARGS...]
  App:
       update [FILE]                 send a new .3dsx (default: ./3dsir.3dsx), the app restarts into it
       put LOCAL [DEST]              copy a file onto the SD card (default dest: sdmc:/3ds/3dsir/NAME)
       get SDPATH [LOCAL]            fetch a file from the SD card (e.g. sdmc:/3dsir.log), stdout without LOCAL
       shot [OUT.png]                grab the top screen (needs Pillow)
       log [N]                       the app log's last N lines (default 50)
       keys KEY [KEY...]             press buttons, one frame each (a b x y l r up down left right start select)
  Sending through ir:u (the normal path):
       send FILE.ir NAME [REPEATS] [clean]   a signal from a Flipper .ir file on the PC
       code PROTOCOL ADDR CMD [REPEATS] [clean]   e.g. code NEC 0x04 0x08
                                     (dense carrier by default: strongest, tested on TVs; a Flipper
                                     right next to the port only decodes the clean one)
       trim US                       shorten every mark by US (the next space grows) for later sends, both
                                     styles (default: clean 80, dense 0)
       irusend BITRATE HEX           raw UART bytes at an IRU_SetBitRate value (3..18)
  SC16IS750 direct (shares the ir module's own i2c::IR session; release puts the chip back as it was):
       takeover [kill] [gpio]        kill: stop the ir module instead (release then REBOOTS the console);
                                     gpio: also open gpio:IR (gpio crashes when that session closes)
       release
       regs                          dump every register (special + enhanced sets, LCR restored)
       rd REG [N]                    read N bytes of register REG (RHR: FIFO bytes)
       wr REG HEX                    write bytes to register REG (THR: into the FIFO)
       uart DIV [irda|nrz] [pre4]    baud = 1152000 / DIV (/4 more with pre4), 8N1, FIFOs reset, TX/RX on
       tx HEX | txfill BYTE COUNT    push bytes through the TX FIFO, report gaps
       rx MS                         poll and drain the RX FIFO for MS milliseconds, print the timeline
       bench [N]                     I2C single-register read round trip
       gpio READMASK [VALUE MASK]    gpio:IR GetData (after SetData when VALUE MASK given)
       i2c DEV RLEN [HEX]            raw I2C write HEX (if any) then read RLEN on device DEV
       ipc i2c|gpio OUTSIZE WORD...  raw IPC on the i2c::IR / gpio:IR session (WORD[0] = header)
"""
import os
import socket
import struct
import sys
import time

PORT = 8020
(OP_PING, OP_TAKEOVER, OP_RELEASE, OP_REG_READ, OP_REG_WRITE, OP_GPIO, OP_I2C, OP_IPC, OP_UPDATE,
 OP_UPDATE_DONE, OP_PUT_FILE, OP_BENCH, OP_TX, OP_RX, OP_SEND, OP_IRU_SEND) = range(16)
OP_SET_TRIM = 0x10
OP_SHOT, OP_KEYS, OP_LOG, OP_GET_FILE = 0x20, 0x21, 0x24, 0x25
CHUNK = 0x7000
ROOT = "sdmc:/3ds/3dsir/"
KEYS = {"a": 1 << 0, "b": 1 << 1, "select": 1 << 2, "start": 1 << 3, "right": 1 << 4, "left": 1 << 5,
        "up": 1 << 6, "down": 1 << 7, "r": 1 << 8, "l": 1 << 9, "x": 1 << 10, "y": 1 << 11}

RHR, IER, IIR, LCR, MCR, LSR, MSR, SPR, TXLVL, RXLVL, IODIR, IOSTATE, IOINTENA, _, IOCONTROL, EFCR = range(16)
THR, FCR = RHR, IIR
DLL, DLH = 0, 1  # LCR[7] = 1
EFR = 2  # LCR = 0xBF
REG_NAMES = {IER: "IER", IIR: "IIR", LCR: "LCR", MCR: "MCR", LSR: "LSR", MSR: "MSR/TCR", SPR: "SPR/TLR",
             TXLVL: "TXLVL", RXLVL: "RXLVL", IODIR: "IODir", IOSTATE: "IOState", IOINTENA: "IOIntEna",
             IOCONTROL: "IOCtl", EFCR: "EFCR"}


class IrError(Exception):
    pass


class Ir:
    def __init__(self, host, port=PORT):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise IrError("connection closed")
            buf += chunk
        return buf

    def call(self, op, payload=b""):
        self.sock.sendall(struct.pack("<BH", op, len(payload)) + payload)
        rop, res, ln = struct.unpack("<BIH", self._recv(7))
        data = self._recv(ln)
        if rop != op:
            raise IrError(f"reply for op {rop}, sent {op}")
        if res:
            raise IrError(f"op {op:#x} failed: {res:08X}")
        return data

    # app
    def ping(self):
        return self.call(OP_PING).decode()

    def update(self, data, log=print):
        for o in range(0, len(data), CHUNK):
            self.call(OP_UPDATE, struct.pack("<I", o) + data[o:o + CHUNK])
            log(f"  {min(o + CHUNK, len(data))}/{len(data)}")
        self.call(OP_UPDATE_DONE, struct.pack("<I", len(data)))

    def put_file(self, path, data):
        p = path.encode()
        for o in range(0, max(len(data), 1), CHUNK):
            self.call(OP_PUT_FILE, struct.pack("<IB", o, len(p)) + p + data[o:o + CHUNK])

    def shot(self):
        from PIL import Image
        raw = b""
        while True:
            chunk = self.call(OP_SHOT, struct.pack("<I", len(raw)))
            if not chunk:
                break
            raw += chunk
        bpp = len(raw) // (240 * 400)  # the console draws in RGB565, other formats are BGR8 / ABGR8
        mode = {2: ("BGR;16", "RGB"), 3: ("BGR", "RGB"), 4: ("ABGR", "RGBA")}[bpp]
        img = Image.frombytes(mode[1], (240, 400), raw, "raw", mode[0])
        return img.transpose(Image.Transpose.ROTATE_90)

    def get_file(self, path):
        data = b""
        while True:
            chunk = self.call(OP_GET_FILE, struct.pack("<I", len(data)) + path.encode())
            if not chunk:
                return data
            data += chunk

    def set_trim(self, us):
        return struct.unpack("<i", self.call(OP_SET_TRIM, struct.pack("<i", us)))[0]

    def log(self, lines=50):
        return self.call(OP_LOG, struct.pack("<I", lines)).decode(errors="replace")

    def keys(self, mask):
        self.call(OP_KEYS, struct.pack("<I", mask))

    # ir:u
    def send_parsed(self, protocol, address, command, repeats=1, clean=False):
        p = protocol.encode()[:15].ljust(16, b"\0")
        return struct.unpack("<I", self.call(OP_SEND, bytes([0, clean, repeats]) + p +
                                             struct.pack("<II", address, command)))[0]

    def send_raw(self, freq, timings, repeats=1, clean=False):
        return struct.unpack("<I", self.call(OP_SEND, bytes([1, clean, repeats]) +
                                             struct.pack("<IH", freq, len(timings)) +
                                             struct.pack(f"<{len(timings)}I", *timings)))[0]

    def iru_send(self, bitrate, data):
        return struct.unpack("<I", self.call(OP_IRU_SEND, bytes([bitrate]) + data))[0]

    # SC16IS750
    def takeover(self, kill=False, gpio=False):
        """Steals the ir module's i2c::IR session by default; kill stops the module instead."""
        self.call(OP_TAKEOVER, bytes([(1 if kill else 4) | gpio << 1]))

    def release(self):
        self.call(OP_RELEASE)

    def rd(self, reg, n=1):
        return self.call(OP_REG_READ, bytes([reg, n]))

    def rd1(self, reg):
        return self.rd(reg)[0]

    def wr(self, reg, data):
        self.call(OP_REG_WRITE, bytes([reg]) + (bytes([data]) if isinstance(data, int) else data))

    def regs(self):
        out = {name: self.rd1(reg) for reg, name in REG_NAMES.items()}
        lcr = out["LCR"]
        self.wr(LCR, 0x83 if lcr == 0xBF else lcr | 0x80)
        out["DLL"], out["DLH"] = self.rd1(DLL), self.rd1(DLH)
        self.wr(LCR, 0xBF)
        out["EFR"] = self.rd1(EFR)
        out["XON1"], out["XON2"], out["XOFF1"], out["XOFF2"] = (self.rd1(r) for r in range(4, 8))
        self.wr(LCR, lcr)
        return out

    def uart(self, div, irda=True, pre4=False):
        """8N1 at 1152000 / div (/4 with pre4), IrDA or plain NRZ, FIFOs on and reset, TX and RX enabled."""
        lcr = 0x03
        self.wr(LCR, 0xBF)
        efr = self.rd1(EFR)
        self.wr(EFR, efr | 0x10)  # enhanced functions: MCR[7:5, 2] writable
        self.wr(LCR, 0x80 | lcr)
        self.wr(DLL, div & 0xFF)
        self.wr(DLH, div >> 8)
        self.wr(LCR, lcr)
        mcr = self.rd1(MCR) & 0x3F
        self.wr(MCR, mcr | (0x40 if irda else 0) | (0x80 if pre4 else 0))
        self.wr(FCR, 0x07)
        self.wr(EFCR, self.rd1(EFCR) & ~0x06)  # the ir module leaves TX and RX disabled when idle
        self.wr(IOSTATE, self.rd1(IOSTATE) & ~1)  # and the transceiver shut down (GPIO0 = its SD pin)

    def tx(self, data):
        d = self.call(OP_TX, data)  # us, FIFO writes, times found empty, bytes sent
        return struct.unpack("<IIII", d) if len(d) >= 16 else struct.unpack("<III", d) + (len(data),)

    def rx(self, ms):
        d = self.call(OP_RX, struct.pack("<H", ms))
        polls, us, n = struct.unpack("<III", d[:12])
        samples, o = [], 12
        for _ in range(n):
            t, k = struct.unpack("<IB", d[o:o + 5])
            samples.append((t, d[o + 5:o + 5 + k]))
            o += 5 + k
        return polls, us, samples

    def bench(self, n=200):
        return struct.unpack("<III", self.call(OP_BENCH, struct.pack("<H", n)))

    def gpio(self, read_mask, value=0, mask=0):
        return struct.unpack("<I", self.call(OP_GPIO, struct.pack("<III", value, mask, read_mask)))[0]

    def i2c(self, dev, rlen, data=b""):
        return self.call(OP_I2C, struct.pack("<BH", dev, rlen) + data)

    def ipc(self, target, out_size, words):
        d = self.call(OP_IPC, struct.pack("<BH", target, out_size) + struct.pack(f"<{len(words)}I", *words))
        return struct.unpack("<16I", d[:64]), d[64:]


def load_ir(path):
    """Flipper .ir file -> list of dicts (name, type, protocol, address, command, frequency, data)."""
    sigs, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#") or ":" not in line:
            continue
        k, v = (x.strip() for x in line.split(":", 1))
        if k == "name":
            cur = {"name": v}
            sigs.append(cur)
        elif cur is not None:
            cur[k] = v
    return sigs


def le_hex(s):
    return int.from_bytes(bytes.fromhex(s.replace(" ", ""))[:4].ljust(4, b"\0"), "little")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ir = Ir(sys.argv[1])
    ir.ping()
    what, args = sys.argv[2], sys.argv[3:]
    clean = "clean" in args
    args = [a for a in args if a not in ("clean", "dense")]
    if what == "update":
        path = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "3dsir.3dsx")
        ir.update(open(path, "rb").read())
        print("updated, app restarting")
    elif what == "put":
        dest = args[1] if len(args) > 1 else ROOT + os.path.basename(args[0])
        ir.put_file(dest, open(args[0], "rb").read())
        print("put", dest)
    elif what == "trim":
        print(f"mark trim {int(args[0])} us (was {ir.set_trim(int(args[0]))})")
    elif what == "get":
        data = ir.get_file(args[0])
        if len(args) > 1:
            open(args[1], "wb").write(data)
            print(f"{len(data)} bytes -> {args[1]}")
        else:
            sys.stdout.write(data.decode(errors="replace"))
    elif what == "shot":
        out = args[0] if args else "shot.png"
        ir.shot().save(out)
        print("saved", out)
    elif what == "log":
        lines = ir.log(int(args[0]) if args else 50).splitlines()
        print("\n".join(l for l in lines if not l.startswith("net: client")))
    elif what == "keys":
        for k in args:
            ir.keys(KEYS[k.lower()])
            time.sleep(0.1)
    elif what == "send":
        sigs = [s for s in load_ir(args[0]) if s["name"] == args[1]]
        if not sigs:
            sys.exit(f"no signal named {args[1]!r}")
        s, rep = sigs[0], int(args[2]) if len(args) > 2 else 3
        if s.get("type") == "raw":
            n = ir.send_raw(int(s.get("frequency", 38000)), [int(x) for x in s["data"].split()], rep, clean)
        else:
            n = ir.send_parsed(s["protocol"], le_hex(s["address"]), le_hex(s["command"]), rep, clean)
        print(f"sent {s['name']}: {n} UART bytes")
    elif what == "code":
        rep = int(args[3]) if len(args) > 3 else 3
        n = ir.send_parsed(args[0], int(args[1], 0), int(args[2], 0), rep, clean)
        print(f"sent: {n} UART bytes")
    elif what == "irusend":
        print(f"on air after {ir.iru_send(int(args[0]), bytes.fromhex(args[1]))} us")
    elif what == "takeover":
        ir.takeover(kill="kill" in args, gpio="gpio" in args)
        print("taken over")
    elif what == "release":
        try:
            ir.release()
            print("released")
        except (IrError, OSError):
            print("connection dropped: rebooting (expected after takeover kill)")
    elif what == "regs":
        r = ir.regs()
        for k, v in r.items():
            print(f"  {k:9} {v:02X}  {v:08b}")
        div = r["DLL"] | r["DLH"] << 8
        pre = 4 if r["MCR"] & 0x80 else 1
        if div:
            print(f"  baud {1152000 / pre / div:.0f} (div {div}{', /4 prescaler' if pre == 4 else ''}),"
                  f" {'IrDA' if r['MCR'] & 0x40 else 'NRZ'}")
    elif what == "rd":
        print(ir.rd(int(args[0], 0), int(args[1], 0) if len(args) > 1 else 1).hex(" "))
    elif what == "wr":
        ir.wr(int(args[0], 0), bytes.fromhex(args[1]))
    elif what == "uart":
        div = int(args[0], 0)
        ir.uart(div, irda="nrz" not in args, pre4="pre4" in args)
        print(f"baud {1152000 / (4 if 'pre4' in args else 1) / div:.0f}")
    elif what in ("tx", "txfill"):
        data = bytes.fromhex(args[0]) if what == "tx" else bytes([int(args[0], 0)]) * int(args[1], 0)
        us, writes, empty, sent = ir.tx(data)
        drained = not sent & 0x80000000
        sent &= 0x7FFFFFFF
        print(f"{sent} bytes in {us} us ({sent * 10e6 / max(us, 1):.0f} bit/s on the wire),"
              f" {writes} FIFO writes, FIFO found empty {empty}x")
        if not drained:
            print("NOT DRAINED: LSR never showed TX empty (transmitter disabled? EFCR bit 2)")
        if sent < len(data):
            print(f"STALLED: TXLVL stayed 0 for 2 s, {len(data) - sent} bytes not queued")
    elif what == "rx":
        polls, us, samples = ir.rx(int(args[0]))
        print(f"{polls} polls in {us} us ({us / max(polls, 1):.0f} us/poll), {len(samples)} with data,"
              f" {sum(len(b) for _, b in samples)} bytes")
        last = None
        for t, b in samples:
            gap = f"+{t - last:6}" if last is not None else " " * 7
            print(f"  {t:9} {gap} {len(b):3}  {b.hex(' ')}")
            last = t
    elif what == "bench":
        mn, avg, mx = ir.bench(int(args[0]) if args else 200)
        print(f"I2C register read: min {mn} us, avg {avg} us, max {mx} us")
    elif what == "gpio":
        v = ir.gpio(int(args[0], 0), *(int(a, 0) for a in args[1:3]))
        print(f"{v:08X}")
    elif what == "i2c":
        print(ir.i2c(int(args[0], 0), int(args[1], 0), bytes.fromhex(args[2]) if len(args) > 2 else b"").hex(" "))
    elif what == "ipc":
        words, data = ir.ipc({"i2c": 0, "gpio": 1}[args[0]], int(args[1], 0), [int(a, 0) for a in args[2:]])
        print(" ".join(f"{w:08X}" for w in words))
        if data:
            print(data.hex(" "))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
