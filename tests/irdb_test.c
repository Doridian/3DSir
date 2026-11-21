// Host checks for the remote index and the finder (irdb.c).
//
//   cc -O2 -Isource tests/irdb_test.c source/irdb.c source/irdecode.c source/irenc.c source/irfile.c -o irdb_test
//   ./irdb_test                   # a few made-up remotes
//   ./irdb_test path/to/Flipper-IRDB   # the real thing: every remote it can detect, found by simulated presses
//
// The simulated user answers each question with their remote's code for that button, or skips it if the
// remote (its file) has none, the way someone holding it would.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "irdb.h"
#include "irenc.h"

static int fails;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if(!(c)) {                                                                                                     \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while(0)

// Does candidate c send the same codes as the true remote t for every button both know? (Then the finder
// can't tell them apart, and either is a right answer.)
static const IrKey* code_for(const IrdbFile* f, int b) {
    for(uint32_t i = 0; i < f->nkeys; i++)
        if(f->keys[i].button == b) return &f->keys[i];
    return NULL;
}

static bool equivalent(const IrdbFile* c, const IrdbFile* t) {
    for(int b = 0; b < irdb_button_count(); b++) {
        const IrKey *x = code_for(c, b), *y = code_for(t, b);
        if(x && y && !irdb_same_code(x, y)) return false;
    }
    return true;
}

typedef struct {
    size_t presses, left;
    bool found, first; // the true remote among the candidates; ranked first
    bool ambiguous; // more than one left, not all of them equivalent
} Outcome;

// What the 3DS would make of a remote sending k: its waveform reduced to mark starts, each late or early by up
// to 150 us (most of the IR port's polling jitter), decoded. Turns out as k itself, or the finder couldn't
// work on real captures.
static uint32_t rng = 12345;
static size_t capture_misses;

static IrKey capture(const IrKey* k) {
    IrTimings tm;
    ir_timings_init(&tm);
    static uint32_t t[600];
    size_t n = 0;
    if(ir_encode_parsed(irdb_proto_name(k->proto), k->address, k->command, &tm)) {
        uint32_t at = 100000;
        for(size_t i = 0; i < tm.n && n < 600; i++) {
            rng = rng * 1103515245 + 12345;
            if(i % 2 == 0) t[n++] = at + (int)((rng >> 8) % 301) - 150;
            at += tm.t[i];
        }
    }
    ir_timings_free(&tm);
    IrDecoded d;
    IrKey got = {0};
    if(!ir_decode(t, n, &d) || !irdb_decoded_key(&d, &got) || !irdb_same_code(&got, k)) {
        if(capture_misses++ < 10)
            printf("capture of %s %X/%X decoded as %s %X/%X\n", irdb_proto_name(k->proto), k->address, k->command,
                   irdb_proto_name(got.proto), got.address, got.command);
    }
    got.button = k->button;
    return got;
}

static size_t wrong_button; // true presses taken for another button (irdb_find_known_as)
static double after[8];
static size_t afterruns[8];

static Outcome find(const IrdbFile* t) {
    Outcome o = {0};
    irdb_find_reset();
    // First the Power button, or whichever one it has.
    const IrKey* k = NULL;
    for(uint32_t i = 0; i < t->nkeys && !k; i++)
        if(t->keys[i].button == irdb_button_id("Power")) k = &t->keys[i];
    if(!k) k = &t->keys[0];
    IrKey c0 = capture(k);
    wrong_button += irdb_find_known_as(&c0, k->button) >= 0;
    if(!irdb_find_press(&c0, k->button)) return o;
    o.presses = 1;
    {
        const IrdbFile* const* cc;
        after[1] += irdb_find_candidates(&cc), afterruns[1]++;
    }
    int b;
    while((b = irdb_find_next()) >= 0) {
        const IrKey* c = NULL;
        for(uint32_t i = 0; i < t->nkeys && !c; i++)
            if(t->keys[i].button == b) c = &t->keys[i];
        if(!c) {
            irdb_find_skip(b);
            continue;
        }
        IrKey cb = capture(c);
        wrong_button += irdb_find_known_as(&cb, b) >= 0;
        if(!irdb_find_press(&cb, b)) {
            irdb_find_skip(b);
            continue;
        }
        o.presses++;
        const IrdbFile* const* cc;
        size_t left = irdb_find_candidates(&cc);
        if(o.presses < 8) after[o.presses] += left, afterruns[o.presses]++;
    }
    const IrdbFile* const* cand;
    o.left = irdb_find_candidates(&cand);
    for(size_t i = 0; i < o.left; i++) {
        if(cand[i] == t) o.found = true, o.first = i == 0;
        if(!equivalent(cand[i], t)) o.ambiguous = true;
    }
    return o;
}

static void write_file(const char* path, const char* text) {
    FILE* fp = fopen(path, "w");
    fputs(text, fp);
    fclose(fp);
}

#define SIG(name, proto, a, c) "name: " name "\ntype: parsed\nprotocol: " proto "\naddress: " a "\ncommand: " c "\n"

static void made_up(void) {
    char dir[] = "/tmp/irdb_testXXXXXX";
    if(!mkdtemp(dir)) return;
    char p[300], cache[300], sub[300], cmd[400];
    const IrdbFile* const* cand;
    size_t n;
    snprintf(sub, sizeof(sub), "%s/TVs", dir);
    mkdir(sub, 0777);
    // A, B and C share their Power code; B and C differ in Vol+. D is a raw capture of A's Power and Mute.
    snprintf(p, sizeof(p), "%s/TVs/A.ir", dir);
    write_file(p, "Filetype: IR signals file\nVersion: 1\n" SIG("Power", "NEC", "04 00 00 00", "08 00 00 00")
                      SIG("Vol_up", "NEC", "04 00 00 00", "02 00 00 00") SIG("Mute", "NEC", "04 00 00 00", "09 00 00 00"));
    snprintf(p, sizeof(p), "%s/TVs/B.ir", dir);
    write_file(p, "Filetype: IR signals file\nVersion: 1\n" SIG("POWER", "NECext", "04 FB 00 00", "08 F7 00 00")
                      SIG("VOL+", "NEC", "04 00 00 00", "12 00 00 00"));
    snprintf(p, sizeof(p), "%s/C.ir", dir);
    write_file(p, "Filetype: IR signals file\nVersion: 1\n" SIG("power", "NEC", "04 00 00 00", "08 00 00 00")
                      SIG("volume_up", "NEC", "04 00 00 00", "22 00 00 00")
                      "name: Other\ntype: parsed\nprotocol: RC5\naddress: 00 00 00 00\ncommand: 0C 00 00 00\n");
    snprintf(p, sizeof(p), "%s/lib.ir", dir);
    write_file(p, "Filetype: IR library file\nVersion: 1\n" SIG("Power", "NEC", "04 00 00 00", "08 00 00 00"));
    snprintf(cache, sizeof(cache), "%s/.irdb", dir);
    snprintf(sub, sizeof(sub), "%s/none.pack", dir);

    irdb_init(dir, cache, sub);
    CHECK(irdb_refresh(NULL, NULL), "refresh");
    CHECK(irdb_files() == 4, "4 files, got %zu", irdb_files());
    CHECK(irdb_detectable() == 3, "3 detectable, got %zu", irdb_detectable());

    // NECext 04FB/08F7 is NEC 04/08 on the air.
    IrKey k = {.proto = 1, .address = 4, .command = 8, .button = IRDB_BTN_OTHER};
    irdb_find_reset();
    // Wrong buttons: Power's code when asked for Vol+ is Power; asked for Power it's right.
    CHECK(irdb_find_known_as(&k, irdb_button_id("Vol+")) == irdb_button_id("Power"), "Power pressed for Vol+");
    CHECK(irdb_find_known_as(&k, irdb_button_id("Power")) < 0, "Power pressed for Power");
    CHECK(irdb_find_press(&k, irdb_button_id("Power")), "power press");
    n = irdb_find_candidates(&cand);
    CHECK(n == 3, "3 candidates (library excluded), got %zu", n);
    int b = irdb_find_next();
    CHECK(b == irdb_button_id("Vol+"), "asks Vol+, got %s", irdb_button_label(b));
    IrKey mute = {.proto = 1, .address = 4, .command = 9};
    CHECK(irdb_find_known_as(&mute, b) == irdb_button_id("Mute"), "A's Mute pressed for Vol+");
    IrKey unnamed = {.proto = 1, .address = 0x55, .command = 0x55};
    CHECK(irdb_find_known_as(&unnamed, b) < 0, "a code nobody has isn't a wrong button");
    IrKey v = {.proto = 1, .address = 4, .command = 0x12};
    CHECK(irdb_find_known_as(&v, b) < 0, "B's Vol+ pressed for Vol+");
    CHECK(irdb_find_press(&v, b), "vol+ press");
    n = irdb_find_candidates(&cand);
    CHECK(n == 1 && strstr(cand[0]->path, "/B.ir"), "B found (%zu left)", n);
    CHECK(irdb_find_next() < 0, "nothing more to ask");
    IrKey none = {.proto = 1, .address = 0x55, .command = 0x55};
    CHECK(!irdb_find_press(&none, IRDB_BTN_OTHER), "a code no candidate has is refused");

    // The cache: a second run reads it back; a touched file is indexed again.
    irdb_init(dir, cache, sub);
    CHECK(irdb_refresh(NULL, NULL) && irdb_files() == 4, "cache reload, %zu", irdb_files());
    snprintf(p, sizeof(p), "%s/TVs/A.ir", dir);
    write_file(p, "Filetype: IR signals file\nVersion: 1\n" SIG("Power", "Samsung32", "07 00 00 00", "02 00 00 00"));
    irdb_touched(p);
    snprintf(p, sizeof(p), "%s/C.ir", dir);
    remove(p);
    CHECK(irdb_refresh(NULL, NULL) && irdb_files() == 3, "C gone, %zu", irdb_files());
    IrKey s = {.proto = 5, .address = 7, .command = 2};
    irdb_find_reset();
    CHECK(irdb_find_press(&s, IRDB_BTN_OTHER), "A re-indexed");

    // The pack: two remotes in, found by a press, opened by their virtual path; rebuilt from the pack alone.
    char pack[300];
    snprintf(pack, sizeof(pack), "%s/db.pack", dir);
    irdb_init(dir, cache, pack);
    irdb_unload();
    const char* t1 = "Filetype: IR signals file\nVersion: 1\n" SIG("Power", "RCA", "0F 00 00 00", "54 00 00 00");
    const char* t2 = "Filetype: IR signals file\nVersion: 1\n" SIG("Power", "RCA", "0E 00 00 00", "54 00 00 00")
        SIG("Mute", "RCA", "0E 00 00 00", "3F 00 00 00");
    CHECK(irdb_pack_begin("abc123"), "pack begin");
    CHECK(irdb_pack_add("TVs/X/One.ir", t1, strlen(t1)) && irdb_pack_add("TVs/Two.ir", t2, strlen(t2)), "pack add");
    CHECK(irdb_pack_end(true), "pack end");
    char pv[64];
    irdb_pack_version(pv, sizeof(pv));
    CHECK(!strcmp(pv, "abc123") && !strcmp(irdb_version(), "abc123"), "version %s / %s", pv, irdb_version());
    for(int pass = 0; pass < 3; pass++) { // as written, from the cache, rebuilt from the pack
        if(pass == 1) irdb_unload();
        if(pass == 2) {
            remove(cache);
            irdb_unload();
        }
        CHECK(irdb_refresh(NULL, NULL), "refresh %d", pass);
        CHECK(irdb_files() == 5, "pass %d: 3 files + 2 packed, got %zu", pass, irdb_files());
        IrKey r = {.proto = 11, .address = 0x0E, .command = 0x3F};
        irdb_find_reset();
        CHECK(irdb_find_press(&r, IRDB_BTN_OTHER), "pass %d: packed remote found", pass);
        n = irdb_find_candidates(&cand);
        CHECK(n == 1 && !strcmp(cand[0]->path, IRDB_PACKED "/TVs/Two.ir"), "pass %d: %s", pass, n ? cand[0]->path : "-");
        IrFile f;
        CHECK(irdb_load(IRDB_PACKED "/TVs/Two.ir", &f) && f.n == 2 && !strcmp(f.sig[1].name, "Mute"), "pass %d: load", pass);
        irfile_free(&f);
    }
    // A failed pack leaves the old one.
    CHECK(irdb_pack_begin("def456") && irdb_pack_add("Y.ir", t1, strlen(t1)), "second pack");
    CHECK(!irdb_pack_end(false), "abandoned");
    CHECK(irdb_refresh(NULL, NULL) && irdb_files() == 5 && !strcmp(irdb_version(), "abc123"), "old pack kept, %zu %s",
          irdb_files(), irdb_version());

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if(system(cmd)) printf("couldn't remove %s\n", dir);
}

static void buttons(void) {
    const char* same[][2] = {{"VOL_UP", "Vol+"}, {"volume up", "Vol+"}, {"Ch_prev", "Ch-"}, {"CH-", "Ch-"},
                             {"POWER", "Power"}, {"Cursor_Left", "Left"}};
    for(size_t i = 0; i < sizeof(same) / sizeof(same[0]); i++)
        CHECK(irdb_button_id(same[i][0]) == irdb_button_id(same[i][1]) && irdb_button_id(same[i][0]) != IRDB_BTN_OTHER,
              "%s = %s", same[i][0], same[i][1]);
    CHECK(irdb_button_id("Picture_mode") == IRDB_BTN_OTHER, "unknown name");
}

static void real(const char* dir) {
    char cache[] = "/tmp/irdb_test_cacheXXXXXX";
    int fd = mkstemp(cache);
    if(fd >= 0) close(fd);
    remove(cache);
    irdb_init(dir, cache, "/nonexistent.pack");
    irdb_refresh(NULL, NULL);
    remove(cache);
    printf("%zu files, %zu remotes can be detected\n", irdb_files(), irdb_detectable());
    size_t runs = 0, found = 0, first = 0, unique = 0, ambiguous = 0, presses = 0, hist[8] = {0};
    for(size_t i = 0; i < irdb_files(); i++) {
        const IrdbFile* t = irdb_file(i);
        if(!t->nkeys || t->library) continue;
        Outcome o = find(t);
        runs++;
        found += o.found;
        first += o.first;
        unique += o.left == 1;
        ambiguous += o.ambiguous;
        presses += o.presses;
        hist[o.presses < 7 ? o.presses : 7]++;
        if(!o.found) printf("not found: %s\n", t->path);
    }
    printf("%zu remotes: found %zu (%.1f%%), ranked first %zu (%.1f%%), alone %zu, left with ones that differ "
           "somewhere %zu (%.1f%%), %.2f presses on average\n",
           runs, found, 100.0 * found / runs, first, 100.0 * first / runs, unique, ambiguous, 100.0 * ambiguous / runs,
           (double)presses / runs);
    printf("presses:");
    for(int i = 1; i < 8; i++) printf(" %d%s: %zu", i, i == 7 ? "+" : "", hist[i]);
    printf("\n");
    printf("candidates left on average after press:");
    for(int i = 1; i < 8; i++) printf(" %d: %.1f", i, afterruns[i] ? after[i] / afterruns[i] : 0);
    printf("\n");
    CHECK(found == runs, "every remote found");
    CHECK(!wrong_button, "%zu true presses taken for the wrong button", wrong_button);
    CHECK(!capture_misses, "%zu jittered captures decoded as something else", capture_misses);
}

int main(int argc, char** argv) {
    buttons();
    made_up();
    if(argc > 1) real(argv[1]);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
