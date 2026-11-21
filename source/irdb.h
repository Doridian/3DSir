#pragma once

// The remote index behind "Find my remote": every .ir file under the remotes folder (the Flipper-IRDB and
// the user's own), reduced to the codes the 3DS's receiver can tell apart, and a guessing game over it.
//
// A code's key is what irdecode.c makes of it: a database signal (parsed or raw) is encoded to a waveform,
// cut down to its mark starts and decoded, the same way a capture from the IR port is. So "NECext
// 04FB/08F7" in a file and the NEC 04/08 a remote sends are the same key, and a raw capture of an NEC
// remote matches too. Codes the receiver can't follow (RC5, RC6, air conditioners' long frames) have no
// key and can't be detected.
//
// Plain C (stdio, dirent): tests/irdb_test.c runs it on a PC against the real Flipper-IRDB.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "irdecode.h"
#include "irfile.h"

typedef struct {
    uint32_t address, command;
    uint8_t proto; // 1-based, irdb_proto_name; 0: none
    uint8_t button; // IRDB_BTN_*, or IRDB_BTN_OTHER
} IrKey;

#define IRDB_BTN_OTHER 0xFF

// Codes as keys: false if the 3DS couldn't receive it.
bool irdb_signal_key(const IrSignal* s, IrKey* k);
bool irdb_decoded_key(const IrDecoded* d, IrKey* k);
bool irdb_same_code(const IrKey* a, const IrKey* b);
const char* irdb_proto_name(uint8_t proto);

// The well-known buttons the finder asks for. A file's own names are mapped onto them ("VOL_UP", "Vol+",
// "volume_up" are all Vol+); anything else is IRDB_BTN_OTHER.
int irdb_button_id(const char* name);
const char* irdb_button_label(int id); // "Vol+"
int irdb_button_count(void);

// ---------------------------------------------------------------- the index

// The downloaded Flipper-IRDB is one pack file, not 9000 files in 4000 folders: on a 3DS a new folder costs
// ~240 ms and a small file ~60 ms, which made unpacking a 25 minute job. Its remotes have virtual paths
// under IRDB_PACKED ("irdb:/TVs/LG/LG_TV.ir"), opened with irdb_load.
//
// The pack: "3DSIRPK1", the version (the commit, NUL-padded to 56 bytes), the .ir files' text back to back,
// then its table of contents (each file: u16 path length, path relative to the database, u32 offset, u32
// length) and a trailer: u32 files, u32 where the table starts, "3DSIRPKE".
#define IRDB_PACKED "irdb:"

typedef struct {
    char* path; // full path, or IRDB_PACKED "/..." in the pack
    IrKey* keys; // its signals that have one
    uint32_t nkeys;
    uint32_t off, len; // in the pack
    uint16_t buttons; // all its signals
    bool library; // an "IR library file" (a universal remote's every Power code): never a match
} IrdbFile;

// root: the folder walked; cache: where the index is kept between runs; pack: the downloaded database.
void irdb_init(const char* root, const char* cache, const char* pack);
bool irdb_loaded(void);
// Just the cache, if nothing is loaded yet.
void irdb_open(void);
// Forgets what's in memory: the next refresh starts from the cache again.
void irdb_unload(void);
// Loads the cache (if any), then brings it up to date: indexes files that are new or touched, drops the
// ones that are gone, and the pack's remotes if the pack changed. progress(done, todo) for the files
// indexed. Saves the cache if anything changed. Returns false if *cancel stopped it (what was indexed so
// far is kept).
bool irdb_refresh(volatile bool* cancel, void (*progress)(size_t done, size_t todo));
// Forgets everything and indexes every file again.
bool irdb_rebuild(volatile bool* cancel, void (*progress)(size_t done, size_t todo));
bool irdb_save(void);
// A file the app wrote, renamed or deleted: indexed again on the next refresh (kept on the SD card, so a
// refresh in a later run still sees it).
void irdb_touched(const char* path);

// Adds (or replaces) a file, already parsed.
void irdb_add(const char* path, const IrFile* f);
// Opens a remote: a file, or one in the pack.
bool irdb_load(const char* path, IrFile* out);
bool irdb_is_packed(const char* path);

// The pack's version (its commit), as recorded by the index; and as the pack file says (empty if none).
const char* irdb_version(void);
void irdb_pack_version(char* out, size_t max);

// Writing a new pack, indexing each remote as it goes in: begin (to a temporary file), add every .ir file
// (rel: its path in the database, "TVs/LG/LG_TV.ir"), end. end(true) writes the table of contents, puts the
// pack in place of the old one and saves the index; end(false) throws it away (the index is reloaded from
// its cache, which still matches the old pack).
bool irdb_pack_begin(const char* version);
bool irdb_pack_add(const char* rel, const char* text, size_t len);
bool irdb_pack_end(bool ok);

size_t irdb_files(void); // files indexed
size_t irdb_detectable(void); // of them, remotes with at least one key
const IrdbFile* irdb_file(size_t i);

// ---------------------------------------------------------------- finding a remote

// A new search: every remote is a candidate.
void irdb_find_reset(void);
// The remote sent this code (asked: the IRDB_BTN_* the user was asked for, or IRDB_BTN_OTHER). The
// candidates become the remotes that have the most of the codes pressed so far. Returns false (and changes
// nothing) if no remote at all has this code with the earlier ones.
bool irdb_find_press(const IrKey* k, int asked);
// Asked for a button, the remote sent a code that the candidates (every remote, before the first press) only
// know as another well-known button: that button (the one most of them call it), the user pressed the wrong
// one. -1 if some candidate has it as the button asked for, or none has it under a well-known name.
int irdb_find_known_as(const IrKey* k, int asked);
// The user's remote has no such button: not asked again.
void irdb_find_skip(int button);
// The button that best tells the candidates apart (fewest expected left after it), -1 if none helps.
int irdb_find_next(void);
size_t irdb_find_presses(void);
// The candidates, best first: the most presses found under the button they were asked as, then the most
// buttons. Valid until the next press.
size_t irdb_find_candidates(const IrdbFile* const** out);
