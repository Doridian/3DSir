#!/usr/bin/env python3
"""Drive a Flipper Zero's IR over its USB CLI, as the other end for testing the 3DS port.

usage: flipper.py [-p PORT] COMMAND [ARGS...]
  tx PROTOCOL ADDR CMD          ir tx, e.g. tx NEC 0x04 0x08
  txraw FREQ DUTY T1 T2 ...     ir tx RAW F:FREQ DC:DUTY (duty in %, timings in us)
  rx SECONDS [raw]              ir rx for that long, prints what the Flipper decodes
  cli LINE                      any CLI line, prints the reply
PORT defaults to the first /dev/serial/by-id/*Flipper* device.
"""
import glob
import sys
import time

import serial

PROMPT = b">: "


class Flipper:
    def __init__(self, port=None):
        if not port:
            ports = glob.glob("/dev/serial/by-id/*Flipper*")
            if not ports:
                raise SystemExit("no Flipper found")
            port = ports[0]
        self.s = serial.Serial(port, 230400, timeout=0.2)
        self.s.write(b"\r")
        self.read_until_prompt(2)

    def read_until_prompt(self, timeout):
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            buf += self.s.read(4096)
            if buf.endswith(PROMPT):
                break
        return buf

    def cli(self, line, timeout=5):
        self.s.write(line.encode() + b"\r")
        out = self.read_until_prompt(timeout).decode(errors="replace")
        lines = out.replace("\r", "").split("\n")
        # drop the echoed command and the trailing prompt
        return "\n".join(l for l in lines[1:] if not l.startswith(">:")).strip()

    def rx(self, seconds, raw=False):
        self.s.write(b"ir rx" + (b" raw" if raw else b"") + b"\r")
        buf, end = b"", time.time() + seconds
        while time.time() < end:
            buf += self.s.read(4096)
        self.s.write(b"\x03")  # Ctrl+C stops ir rx
        buf += self.read_until_prompt(2)
        lines = buf.decode(errors="replace").replace("\r", "").split("\n")
        return "\n".join(l for l in lines[1:] if l.strip() and not l.startswith(">:")).strip()


def main():
    args = sys.argv[1:]
    port = None
    if args[:1] == ["-p"]:
        port, args = args[1], args[2:]
    if not args:
        raise SystemExit(__doc__)
    f = Flipper(port)
    what, args = args[0], args[1:]
    if what == "tx":
        print(f.cli("ir tx " + " ".join(args)))
    elif what == "txraw":
        print(f.cli(f"ir tx RAW F:{args[0]} DC:{args[1]} " + " ".join(args[2:])))
    elif what == "rx":
        print(f.rx(float(args[0]), raw="raw" in args[1:]))
    elif what == "cli":
        print(f.cli(" ".join(args)))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
