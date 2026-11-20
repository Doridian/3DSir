#!/usr/bin/env python3
"""Draws the app's icon (48x48), the CIA's HOME menu banner (256x128) and its jingle.

The outputs are checked in (icon.png, meta/banner.png, meta/banner.wav); run this only to change them:
    python3 meta/make_art.py [path/to/bold.ttf]
Needs Pillow. A TV remote firing infrared beams, in IR reds and a warm amber on a dark night blue.
"""
import math
import os
import struct
import sys
import wave

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

BG_TOP = (0x2B, 0x10, 0x22)
BG_BOT = (0x08, 0x06, 0x10)
BODY = (0x1C, 0x1A, 0x26)
EDGE = (0x5A, 0x55, 0x6E)
BUTTON = (0x3C, 0x38, 0x4C)
POWER = (0xFF, 0x3B, 0x4E)
IR = (0xFF, 0x2E, 0x63)
AMBER = (0xFF, 0xA3, 0x3D)
TEXT = (0xF4, 0xEE, 0xF2)


def lerp(a, b, t):
	return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def gradient(w, h, top, bot):
	g = Image.new("RGB", (w, h))
	d = ImageDraw.Draw(g)
	for y in range(h):
		d.line([(0, y), (w, y)], fill=lerp(top, bot, y / max(1, h - 1)))
	return g


def remote_and_beams(size, cx, cy, s, angle=-35):
	"""A TV remote (pointing up-right, tilted by `angle`) with three IR arcs leaving its tip, on a
	transparent layer of `size`. (cx, cy): remote centre, s: remote length."""
	layer = Image.new("RGBA", size, (0, 0, 0, 0))

	# The remote, drawn upright (tip at the top) on its own layer and rotated.
	rw, rh = int(s * 0.36), int(s)
	pad = int(s * 0.3)
	rem = Image.new("RGBA", (rw + 2 * pad, rh + 2 * pad), (0, 0, 0, 0))
	d = ImageDraw.Draw(rem)
	box = (pad, pad, pad + rw, pad + rh)
	d.rounded_rectangle(box, int(rw * 0.42), fill=BODY + (255,), outline=EDGE + (255,), width=max(2, int(s * 0.03)))
	# IR LED window at the tip
	lw, lh = int(rw * 0.42), int(s * 0.05)
	lx, ly = pad + (rw - lw) // 2, pad + int(s * 0.035)
	d.rounded_rectangle((lx, ly, lx + lw, ly + lh), lh // 2, fill=IR + (255,))
	# power button
	pr = rw * 0.13
	px, py = pad + rw * 0.5, pad + s * 0.19
	d.ellipse((px - pr, py - pr, px + pr, py + pr), fill=POWER + (255,))
	# d-pad ring
	rr = rw * 0.26
	py = pad + s * 0.40
	d.ellipse((px - rr, py - rr, px + rr, py + rr), outline=BUTTON + (255,), width=max(2, int(rw * 0.1)))
	d.ellipse((px - rr * 0.3, py - rr * 0.3, px + rr * 0.3, py + rr * 0.3), fill=AMBER + (255,))
	# number grid
	br = rw * 0.075
	for row in range(3):
		for col in range(3):
			bx = pad + rw * (0.27 + col * 0.23)
			by = pad + s * (0.62 + row * 0.1)
			d.ellipse((bx - br, by - br, bx + br, by + br), fill=BUTTON + (255,))
	rem = rem.rotate(angle, resample=Image.BICUBIC, expand=True)
	layer.alpha_composite(rem, (int(cx - rem.width / 2), int(cy - rem.height / 2)))

	# IR arcs from the tip, red -> amber, with a soft glow underneath
	a = math.radians(-angle)  # PIL rotates counter-clockwise; screen y points down
	dx, dy = math.sin(a), -math.cos(a)  # direction the tip points
	ox, oy = cx + dx * s * 0.47, cy + dy * s * 0.47
	heading = math.degrees(math.atan2(dy, dx))
	arcs = Image.new("RGBA", size, (0, 0, 0, 0))
	d = ImageDraw.Draw(arcs)
	w = max(2, int(s * 0.055))
	for i in range(3):
		rad = s * (0.16 + i * 0.13)
		col = lerp(IR, AMBER, i / 2) + (255,)
		d.arc((ox - rad, oy - rad, ox + rad, oy + rad), heading - 38, heading + 38, fill=col, width=w)
	glow = arcs.filter(ImageFilter.GaussianBlur(s * 0.045))
	layer.alpha_composite(glow)
	layer.alpha_composite(arcs)
	return layer


def scanlines(w, h, step, alpha):
	g = Image.new("RGBA", (w, h), (0, 0, 0, 0))
	d = ImageDraw.Draw(g)
	for i in range(0, h, step):
		d.line([(0, i), (w, i)], fill=IR + (alpha,), width=1)
	return g


def icon(path):
	S = 8  # supersampling
	N = 48 * S
	base = gradient(N, N, BG_TOP, BG_BOT).convert("RGBA")
	base.alpha_composite(scanlines(N, N, 4 * S, 18))
	base.alpha_composite(remote_and_beams((N, N), N * 0.36, N * 0.64, N * 0.62))
	# opaque: the SMDH icon has no alpha, the HOME menu draws its own frame
	base.convert("RGB").resize((48, 48), Image.LANCZOS).save(path)


def banner(path, font_path):
	S = 4
	W, H = 256 * S, 128 * S
	base = gradient(W, H, BG_TOP, BG_BOT).convert("RGBA")
	base.alpha_composite(scanlines(W, H, 6 * S, 26))
	base.alpha_composite(remote_and_beams((W, H), W * 0.15, H * 0.62, H * 0.62))

	font = ImageFont.truetype(font_path, 25 * S)
	small = ImageFont.truetype(font_path, 9 * S)
	tx = int(W * 0.39)
	title = Image.new("RGBA", (W, H), (0, 0, 0, 0))
	td = ImageDraw.Draw(title)
	td.text((tx, H * 0.28), "IR", font=font, fill=IR + (255,))
	px = tx + td.textlength("IR ", font=font)
	td.text((px, H * 0.28), "Remote", font=font, fill=TEXT + (255,))
	td.text((tx + S, H * 0.64), "UNIVERSAL TV REMOTE", font=small, fill=AMBER + (255,))
	base.alpha_composite(title.filter(ImageFilter.GaussianBlur(3 * S)))
	base.alpha_composite(title)
	base.convert("RGB").resize((256, 128), Image.LANCZOS).save(path)


def jingle(path):
	"""A remote's click, then a short warm chime, like a TV switching on."""
	rate = 32000
	samples = []
	for f0, f1, dur in ((3200, 2400, 0.02), (0, 0, 0.06), (880, 880, 0.09), (1320, 1320, 0.22)):
		n = int(rate * dur)
		ph = 0.0
		for i in range(n):
			t = i / n
			if f0:
				ph += 2 * math.pi * (f0 + (f1 - f0) * t) / rate
				env = min(1.0, i / (rate * 0.003)) * (1 - t) ** 1.6
				v = (math.sin(ph) + 0.3 * math.sin(2 * ph) + 0.1 * math.sin(3 * ph)) * env * 0.42
			else:
				v = 0.0
			samples.append(v)
	samples += [0.0] * int(rate * 0.3)
	with wave.open(path, "wb") as w:
		w.setnchannels(1)
		w.setsampwidth(2)
		w.setframerate(rate)
		w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in samples))


if __name__ == "__main__":
	font = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"
	icon(os.path.join(HERE, "..", "icon.png"))
	banner(os.path.join(HERE, "banner.png"), font)
	jingle(os.path.join(HERE, "banner.wav"))
