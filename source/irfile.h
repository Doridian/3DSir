#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "irenc.h"

typedef struct {
    char name[64];
    bool raw;
    char protocol[16];
    uint32_t address;
    uint32_t command;
    uint32_t frequency;
    char duty[16]; // raw: duty_cycle as written ("0.330000")
    uint32_t* data; // raw timings, mark first
    size_t data_n;
} IrSignal;

typedef struct {
    char filetype[32]; // "IR signals file" or "IR library file"
    IrSignal* sig;
    size_t n;
} IrFile;

// Parses a Flipper Zero "IR signals file" / "IR library file".
bool irfile_load(const char* path, IrFile* out);
void irfile_free(IrFile* f);
void irfile_init(IrFile* f); // an empty signals file
// Writes the file the way the Flipper does (comments in it are not kept). Through a temporary file, so
// a failed write leaves the old one.
bool irfile_save(const IrFile* f, const char* path);

// Editing: add copies s (its raw data too) at the end.
void irfile_add(IrFile* f, const IrSignal* s);
void irfile_remove(IrFile* f, size_t i);
void irfile_move(IrFile* f, size_t from, size_t to); // the others shift to make room
// Replaces signal i's code with s's (keeps its name).
void irfile_replace_code(IrFile* f, size_t i, const IrSignal* s);
// "NEC 04/08", "raw 38 kHz, 67 samples"
void irfile_describe(const IrSignal* s, char* out, size_t max);

// Turns a signal into a waveform, sent `repeats` times back to back (some
// receivers, e.g. Denon, ignore a single frame). False if unsupported.
bool irfile_signal_timings(const IrSignal* s, unsigned repeats, IrTimings* out);
