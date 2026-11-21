#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decoding what the 3DS's IR receiver can tell about a remote's signal: in IrDA mode at 38400 baud the
// SC16IS750 receives one byte per burst of carrier (38/40 kHz), so all there is are the times the marks
// start, to about +-350 us. Pulse-distance codes keep every bit in the time from one mark start to the
// next, and so decode: NEC, NECext, NEC42, NEC42ext, Samsung32, SIRC/15/20, Kaseikyo, Pioneer, RCA.
// SIRC and Pioneer have no stop mark: their last bit can't be seen and is taken as 0 (the usual value).
// RC5 / RC6 (36 kHz, Manchester) arrive as a stream of bytes with no usable edges.

typedef struct {
    char protocol[16];
    uint32_t address, command;
    size_t edges; // mark starts the frame took
    bool last_bit_guessed;
} IrDecoded;

// t: mark start times in microseconds, ascending. Tries every protocol on the first frame and keeps the
// closest fit.
bool ir_decode(const uint32_t* t, size_t n, IrDecoded* out);

// Why nothing decoded, for the user ("36 kHz remote?", "too short").
const char* ir_decode_hint(const uint32_t* t, size_t n);
