#include "irdecode.h"

#include <stdio.h>
#include <string.h>

// A pulse-distance code as its mark-start intervals (mark + following space), from irenc.c's timings.
typedef struct {
    const char* name;
    uint16_t lead, b0, b1; // microseconds
    uint8_t bits;
    bool stop; // a stop mark after the last bit (else that bit's interval runs into the next frame)
} Shape;

static const Shape shapes[] = {
    {"NEC", 13500, 1120, 2250, 32, true},     {"NEC42", 13500, 1120, 2250, 42, true},
    {"Samsung32", 9000, 1100, 2200, 32, true}, {"SIRC", 3000, 1200, 1800, 12, false},
    {"SIRC15", 3000, 1200, 1800, 15, false},  {"SIRC20", 3000, 1200, 1800, 20, false},
    {"Kaseikyo", 5184, 864, 1728, 48, true},  {"Pioneer", 12725, 1000, 2000, 33, false},
    {"RCA", 8000, 1500, 2500, 24, true},
};

#define LEAD_TOL 0.15f // relative
#define BIT_TOL 0.40f
#define BIT_TOL_US 450 // or absolute: short bits are mostly jitter
#define FRAME_GAP 4000 // us: longer than any bit, shorter than any gap between frames

static float rel(uint32_t got, uint32_t want) {
    float d = (float)got - (float)want;
    return (d < 0 ? -d : d) / want;
}

// One frame starting at edge s: adds each bit's vote to v (> 0: a 1, in units of the bit's length) and
// returns the fit (mean squared relative error), < 0 if it isn't one.
static float fit(const Shape* sh, const uint32_t* t, size_t n, size_t s, float* v) {
    size_t edges = 1 + sh->bits + sh->stop;
    if(s + edges > n) return -1;
    if(s && t[s] - t[s - 1] < FRAME_GAP) return -1; // not a frame start
    if(s + edges < n && t[s + edges] - t[s + edges - 1] < FRAME_GAP) return -1; // the frame goes on
    float e = rel(t[s + 1] - t[s], sh->lead);
    if(e > LEAD_TOL) return -1;
    float sum = e * e, vote[64];
    // Each mark start is late by up to a poll or two (+-150 us, a few +-350), which stretches one interval
    // and shrinks the next: bits are picked by where the next mark should start, from a reference that
    // takes up half of every edge's error (the rest is the sender's clock, which drifts over a frame).
    size_t measured = sh->stop ? sh->bits : sh->bits - 1u;
    float ref = t[s + 1];
    for(size_t i = 0; i < measured; i++) {
        float at = t[s + 2 + i], d0 = at - ref - sh->b0, d1 = at - ref - sh->b1;
        if(d0 < 0) d0 = -d0;
        if(d1 < 0) d1 = -d1;
        bool one = d1 < d0;
        float best = one ? d1 : d0, len = one ? sh->b1 : sh->b0;
        if(best > len * BIT_TOL && best > BIT_TOL_US) return -1;
        vote[i] = (d0 - d1) / sh->b0;
        sum += (best / len) * (best / len);
        ref = (ref + len + at) / 2;
    }
    for(size_t i = 0; i < measured; i++) v[i] += vote[i];
    return sum / (measured + 1);
}

static uint64_t bits_le(const uint8_t* d) {
    uint64_t v = 0;
    for(int i = 7; i >= 0; i--) v = v << 8 | d[i];
    return v;
}

// The protocol's fields from its bits, as the Flipper firmware's decoders give them; false when its
// checks fail.
static bool fields(const Shape* sh, const uint8_t* d, IrDecoded* o) {
    uint64_t v = bits_le(d);
    const char* p = sh->name;
    o->last_bit_guessed = false;
    if(!strcmp(p, "NEC")) {
        if(d[1] == (uint8_t)~d[0] && d[3] == (uint8_t)~d[2]) {
            o->address = d[0], o->command = d[2];
        } else {
            p = "NECext";
            o->address = d[0] | d[1] << 8, o->command = d[2] | d[3] << 8;
        }
    } else if(!strcmp(p, "NEC42")) {
        uint32_t a = v & 0x1FFF, na = v >> 13 & 0x1FFF, c = v >> 26 & 0xFF, nc = v >> 34 & 0xFF;
        if(na == (~a & 0x1FFF) && nc == (~c & 0xFF)) {
            o->address = a, o->command = c;
        } else {
            p = "NEC42ext";
            o->address = v & 0x3FFFFFF, o->command = v >> 26 & 0xFFFF;
        }
    } else if(!strcmp(p, "Samsung32")) {
        if(d[1] != d[0] || d[3] != (uint8_t)~d[2]) return false;
        o->address = d[0], o->command = d[2];
    } else if(!strncmp(p, "SIRC", 4)) {
        o->command = v & 0x7F;
        o->address = v >> 7;
        o->last_bit_guessed = true;
    } else if(!strcmp(p, "Kaseikyo")) {
        uint8_t par = d[0] ^ d[1];
        if((d[2] & 0xF) != ((par & 0xF) ^ (par >> 4)) || d[5] != (d[2] ^ d[3] ^ d[4])) return false;
        uint32_t vendor = d[0] | d[1] << 8, id = d[4] >> 6;
        o->address = id << 24 | vendor << 8 | (d[2] >> 4) << 4 | (d[3] & 0xF);
        o->command = (d[3] >> 4) | (d[4] & 0x3F) << 4;
    } else if(!strcmp(p, "Pioneer")) { // the 33rd bit is always 0
        if(d[1] != (uint8_t)~d[0] || d[3] != (uint8_t)~d[2]) return false;
        o->address = d[0], o->command = d[2];
    } else if(!strcmp(p, "RCA")) {
        uint32_t a = v & 0xF, c = v >> 4 & 0xFF, na = v >> 12 & 0xF, nc = v >> 16 & 0xFF;
        if(na != (~a & 0xF) || nc != (~c & 0xFF)) return false;
        o->address = a, o->command = c;
    } else {
        return false;
    }
    snprintf(o->protocol, sizeof(o->protocol), "%s", p);
    o->edges = 1 + sh->bits + sh->stop;
    return true;
}

bool ir_decode(const uint32_t* t, size_t n, IrDecoded* out) {
    float best = -1;
    for(size_t k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
        const Shape* sh = &shapes[k];
        // Every frame that fits (a capture may start inside one), the bits by vote: a remote held down
        // repeats its frame (SIRC sends at least three), and the votes outweigh one frame's bad edge.
        float v[64] = {0}, f = 0;
        int frames = 0;
        for(size_t s = 0; s + 1 + sh->bits + sh->stop <= n; s++) {
            float ff = fit(sh, t, n, s, v);
            if(ff < 0) continue;
            f += ff;
            frames++;
            s += sh->bits; // past this frame
        }
        if(!frames) continue;
        f /= frames;
        uint8_t d[8] = {0};
        for(int i = 0; i < sh->bits; i++)
            if(v[i] > 0) d[i / 8] |= 1u << (i % 8);
        IrDecoded o;
        if(!fields(sh, d, &o)) continue;
        if(best < 0 || f < best) {
            best = f;
            *out = o;
        }
    }
    return best >= 0;
}

const char* ir_decode_hint(const uint32_t* t, size_t n) {
    static char s[160];
    if(!n) return "Nothing received.";
    if(n < 13) {
        snprintf(s, sizeof(s), "Only %zu burst%s of light: hold the remote closer and press its button once.", n,
                 n == 1 ? "" : "s");
        return s;
    }
    size_t shorts = 0;
    for(size_t i = 1; i < n; i++) shorts += t[i] - t[i - 1] < 800;
    if(shorts * 3 > n) return "The bursts come too close together for a supported code: probably a 36 kHz "
                              "(RC5 / RC6) remote, which the 3DS's IrDA receiver can't follow.";
    snprintf(s, sizeof(s), "%zu bursts, but not a code the 3DS can learn (NEC, Samsung, Sony, Kaseikyo, Pioneer, "
             "RCA). Try again.", n);
    return s;
}
