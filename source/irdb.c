#include "irdb.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "irenc.h"

// ---------------------------------------------------------------- keys

static const char* const protos[] = {"NEC",  "NECext", "NEC42",    "NEC42ext", "Samsung32", "SIRC",
                                     "SIRC15", "SIRC20", "Kaseikyo", "Pioneer",  "RCA"};
#define NPROTOS (sizeof(protos) / sizeof(protos[0]))

const char* irdb_proto_name(uint8_t proto) {
    return proto && proto <= NPROTOS ? protos[proto - 1] : "?";
}

bool irdb_decoded_key(const IrDecoded* d, IrKey* k) {
    for(size_t i = 0; i < NPROTOS; i++) {
        if(strcmp(d->protocol, protos[i])) continue;
        k->proto = i + 1;
        k->address = d->address;
        k->command = d->command;
        k->button = IRDB_BTN_OTHER;
        return true;
    }
    return false;
}

#define MAX_EDGES 512

bool irdb_signal_key(const IrSignal* s, IrKey* k) {
    IrTimings tm;
    ir_timings_init(&tm);
    bool ok = irfile_signal_timings(s, 1, &tm);
    if(ok) {
        // What the IR port would see: when each mark starts.
        static uint32_t t[MAX_EDGES];
        size_t n = 0;
        uint32_t at = 0;
        for(size_t i = 0; i < tm.n && n < MAX_EDGES; i++) {
            if(i % 2 == 0) t[n++] = at;
            at += tm.t[i];
        }
        IrDecoded d;
        ok = ir_decode(t, n, &d) && irdb_decoded_key(&d, k);
    }
    ir_timings_free(&tm);
    if(ok) k->button = irdb_button_id(s->name);
    return ok;
}

bool irdb_same_code(const IrKey* a, const IrKey* b) {
    return a->proto == b->proto && a->address == b->address && a->command == b->command;
}

// ---------------------------------------------------------------- buttons

// Names lowercased, with everything but letters, digits, '+' and '-' dropped.
static const struct {
    const char* label;
    const char* names; // space separated
} buttons[] = {
    {"Power", "power pwr onoff poweronoff powertoggle standby"},
    {"Vol+", "vol+ volup volumeup volume+ volplus"},
    {"Vol-", "vol- voldn voldown volumedown volume- volminus voldwn"},
    {"Mute", "mute muting"},
    {"Ch+", "ch+ chup chnext channelup channel+ chanup chplus prog+ progup p+"},
    {"Ch-", "ch- chdn chdown chprev channeldown channel- chandown chminus prog- progdn progdown p-"},
    {"Menu", "menu"},
    {"OK", "ok select"},
    {"Up", "up uparrow cursorup"},
    {"Down", "down dn dnarrow downarrow cursordown"},
    {"Left", "left leftarrow cursorleft"},
    {"Right", "right rightarrow cursorright"},
    {"Back", "back return"},
    {"Exit", "exit"},
    {"Home", "home"},
    {"Info", "info display"},
    {"Input", "input source tvav avtv inputsource"},
    {"Guide", "guide epg"},
    {"Enter", "enter"},
    {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"},
    {"6", "6"}, {"7", "7"}, {"8", "8"}, {"9", "9"}, {"0", "0"},
    {"Play", "play"},
    {"Pause", "pause"},
    {"Play/Pause", "playpause"},
    {"Stop", "stop"},
    {"Fast forward", "ffwd fastforward ff fwd forward"},
    {"Rewind", "rew rewind rev"},
    {"Next", "next"},
    {"Previous", "prev previous"},
    {"Eject", "eject openclose"},
    {"Sleep", "sleep"},
};
#define NBUTTONS (int)(sizeof(buttons) / sizeof(buttons[0]))

int irdb_button_count(void) {
    return NBUTTONS;
}

const char* irdb_button_label(int id) {
    return id >= 0 && id < NBUTTONS ? buttons[id].label : "any button";
}

int irdb_button_id(const char* name) {
    char n[64];
    size_t k = 0;
    for(; *name && k < sizeof(n) - 1; name++) {
        unsigned char c = *name;
        if(isalnum(c) || c == '+' || c == '-') n[k++] = tolower(c);
    }
    n[k] = 0;
    if(!k) return IRDB_BTN_OTHER;
    for(int i = 0; i < NBUTTONS; i++) {
        const char* s = buttons[i].names;
        while(*s) {
            size_t len = strcspn(s, " ");
            if(len == k && !memcmp(s, n, k)) return i;
            s += len;
            while(*s == ' ') s++;
        }
    }
    return IRDB_BTN_OTHER;
}

// ---------------------------------------------------------------- the index

static struct {
    char root[256], cache[256], pack[256], dirty[300];
    char version[64]; // of the pack, when it was indexed
    IrdbFile* f; // sorted by path
    size_t n, cap;
    bool loaded, changed;
} ix;

void irdb_init(const char* root, const char* cache, const char* pack) {
    snprintf(ix.root, sizeof(ix.root), "%s", root);
    snprintf(ix.cache, sizeof(ix.cache), "%s", cache);
    snprintf(ix.pack, sizeof(ix.pack), "%s", pack);
    snprintf(ix.dirty, sizeof(ix.dirty), "%s.dirty", cache);
}

bool irdb_loaded(void) {
    return ix.loaded;
}

size_t irdb_files(void) {
    return ix.n;
}

const IrdbFile* irdb_file(size_t i) {
    return i < ix.n ? &ix.f[i] : NULL;
}

size_t irdb_detectable(void) {
    size_t k = 0;
    for(size_t i = 0; i < ix.n; i++) k += ix.f[i].nkeys && !ix.f[i].library;
    return k;
}

const char* irdb_version(void) {
    return ix.version;
}

static void set_version(const char* v) {
    if(!strcmp(ix.version, v)) return;
    snprintf(ix.version, sizeof(ix.version), "%s", v);
    ix.changed = true;
}

bool irdb_is_packed(const char* path) {
    return !strncmp(path, IRDB_PACKED "/", sizeof(IRDB_PACKED));
}

static void file_free(IrdbFile* f) {
    free(f->path);
    free(f->keys);
}

static void clear(void) {
    for(size_t i = 0; i < ix.n; i++) file_free(&ix.f[i]);
    free(ix.f);
    ix.f = NULL;
    ix.n = ix.cap = 0;
    *ix.version = 0;
}

// Where path is (or would go), by binary search.
static size_t find(const char* path, bool* found) {
    size_t lo = 0, hi = ix.n;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(ix.f[mid].path, path);
        if(!c) {
            *found = true;
            return mid;
        }
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = false;
    return lo;
}

static IrdbFile* slot(const char* path) {
    bool found;
    size_t i = find(path, &found);
    if(found) {
        free(ix.f[i].keys);
        ix.f[i].keys = NULL;
        return &ix.f[i];
    }
    if(ix.n == ix.cap) {
        ix.cap = ix.cap ? ix.cap * 2 : 1024;
        ix.f = realloc(ix.f, ix.cap * sizeof(IrdbFile));
    }
    memmove(&ix.f[i + 1], &ix.f[i], (ix.n - i) * sizeof(IrdbFile));
    ix.n++;
    memset(&ix.f[i], 0, sizeof(IrdbFile));
    ix.f[i].path = strdup(path);
    return &ix.f[i];
}

static IrdbFile* add(const char* path, const IrFile* f) {
    IrdbFile* e = slot(path);
    e->buttons = f->n > 0xFFFF ? 0xFFFF : f->n;
    e->library = !strcmp(f->filetype, "IR library file");
    e->nkeys = 0;
    e->off = e->len = 0;
    if(f->n) e->keys = malloc(f->n * sizeof(IrKey));
    for(size_t i = 0; i < f->n && e->nkeys < 0xFFFF; i++)
        if(irdb_signal_key(&f->sig[i], &e->keys[e->nkeys])) e->nkeys++;
    if(!e->nkeys) {
        free(e->keys);
        e->keys = NULL;
    }
    ix.changed = true;
    return e;
}

void irdb_add(const char* path, const IrFile* f) {
    add(path, f);
}

static void remove_at(size_t i) {
    file_free(&ix.f[i]);
    memmove(&ix.f[i], &ix.f[i + 1], (ix.n - i - 1) * sizeof(IrdbFile));
    ix.n--;
    ix.changed = true;
}

static void remove_packed(void) {
    size_t k = 0;
    for(size_t i = 0; i < ix.n; i++) {
        if(irdb_is_packed(ix.f[i].path)) {
            file_free(&ix.f[i]);
            ix.changed = true;
        } else {
            ix.f[k++] = ix.f[i];
        }
    }
    ix.n = k;
}

// Parses .ir text in memory.
static void parse(const char* text, size_t len, IrFile* out) {
    FILE* m = len ? fmemopen((void*)text, len, "r") : NULL;
    if(!m) {
        irfile_init(out);
        return;
    }
    irfile_read(m, out);
    fclose(m);
}

// ---------------------------------------------------------------- the pack

#define PACK_MAGIC "3DSIRPK1"
#define PACK_END "3DSIRPKE"
#define PACK_HEAD 64
#define PACK_VERSION (PACK_HEAD - 8)

static bool get(FILE* fp, void* p, size_t n) {
    return fread(p, 1, n, fp) == n;
}

static FILE* pack_open(char* version, size_t max) {
    FILE* fp = fopen(ix.pack, "rb");
    if(!fp) return NULL;
    char head[PACK_HEAD];
    if(!get(fp, head, PACK_HEAD) || memcmp(head, PACK_MAGIC, 8)) {
        fclose(fp);
        return NULL;
    }
    if(version) snprintf(version, max, "%.*s", PACK_VERSION, head + 8);
    return fp;
}

void irdb_pack_version(char* out, size_t max) {
    *out = 0;
    FILE* fp = pack_open(out, max);
    if(fp) fclose(fp);
}

bool irdb_load(const char* path, IrFile* out) {
    if(!irdb_is_packed(path)) return irfile_load(path, out);
    bool found;
    size_t i = find(path, &found);
    if(!found) return false;
    FILE* fp = pack_open(NULL, 0);
    if(!fp) return false;
    char* buf = malloc(ix.f[i].len + 1);
    bool ok = buf && !fseek(fp, ix.f[i].off, SEEK_SET) && get(fp, buf, ix.f[i].len);
    fclose(fp);
    if(ok) parse(buf, ix.f[i].len, out);
    free(buf);
    return ok;
}

// Indexes every remote in the pack again.
static bool index_pack(volatile bool* cancel, void (*progress)(size_t, size_t)) {
    remove_packed();
    set_version("");
    char v[PACK_VERSION + 1];
    FILE* fp = pack_open(v, sizeof(v));
    if(!fp) return true;
    static char vbuf[65536];
    setvbuf(fp, vbuf, _IOFBF, sizeof(vbuf));
    uint32_t n, toc;
    char end[8];
    bool ok = !fseek(fp, -16, SEEK_END) && get(fp, &n, 4) && get(fp, &toc, 4) && get(fp, end, 8) &&
              !memcmp(end, PACK_END, 8) && !fseek(fp, toc, SEEK_SET);
    // The table first (it's at the end), then the files in order.
    typedef struct {
        char* rel;
        uint32_t off, len;
    } Entry;
    Entry* e = ok ? calloc(n ? n : 1, sizeof(Entry)) : NULL;
    uint32_t got = 0;
    for(; ok && got < n; got++) {
        uint16_t pl;
        ok = get(fp, &pl, 2) && (e[got].rel = malloc(pl + 1)) && get(fp, e[got].rel, pl);
        if(ok) e[got].rel[pl] = 0;
        ok = ok && get(fp, &e[got].off, 4) && get(fp, &e[got].len, 4);
    }
    char* buf = NULL;
    size_t cap = 0;
    char path[600];
    for(uint32_t i = 0; ok && i < n; i++) {
        if(cancel && *cancel) {
            ok = false;
            break;
        }
        if(progress && i % 8 == 0) progress(i, n);
        if(e[i].len + 1 > cap) buf = realloc(buf, cap = e[i].len + 1);
        ok = !fseek(fp, e[i].off, SEEK_SET) && get(fp, buf, e[i].len);
        if(!ok) break;
        IrFile f;
        parse(buf, e[i].len, &f);
        snprintf(path, sizeof(path), IRDB_PACKED "/%s", e[i].rel);
        IrdbFile* x = add(path, &f);
        x->off = e[i].off;
        x->len = e[i].len;
        irfile_free(&f);
    }
    free(buf);
    for(uint32_t i = 0; e && i < got; i++) free(e[i].rel);
    free(e);
    fclose(fp);
    if(ok) set_version(v);
    return ok;
}

static struct {
    FILE* fp;
    char tmp[300], version[PACK_VERSION + 1];
    uint32_t off;
    char** rel;
    uint32_t *eoff, *elen, n, cap;
} pw;

bool irdb_pack_begin(const char* version) {
    irdb_open();
    snprintf(pw.tmp, sizeof(pw.tmp), "%s.new", ix.pack);
    snprintf(pw.version, sizeof(pw.version), "%s", version);
    pw.fp = fopen(pw.tmp, "wb");
    if(!pw.fp) return false;
    static char buf[256 * 1024]; // a few big writes: the SD card is fast at those
    setvbuf(pw.fp, buf, _IOFBF, sizeof(buf));
    char head[PACK_HEAD] = PACK_MAGIC;
    memcpy(head + 8, version, strnlen(version, PACK_VERSION)); // NUL-padded (head is zeroed past the magic)
    fwrite(head, 1, PACK_HEAD, pw.fp);
    pw.off = PACK_HEAD;
    pw.n = 0;
    remove_packed();
    set_version("");
    return !ferror(pw.fp);
}

bool irdb_pack_add(const char* rel, const char* text, size_t len) {
    if(!pw.fp || fwrite(text, 1, len, pw.fp) != len) return false;
    if(pw.n == pw.cap) {
        pw.cap = pw.cap ? pw.cap * 2 : 1024;
        pw.rel = realloc(pw.rel, pw.cap * sizeof(char*));
        pw.eoff = realloc(pw.eoff, pw.cap * sizeof(uint32_t));
        pw.elen = realloc(pw.elen, pw.cap * sizeof(uint32_t));
    }
    pw.rel[pw.n] = strdup(rel);
    pw.eoff[pw.n] = pw.off;
    pw.elen[pw.n++] = len;
    IrFile f;
    parse(text, len, &f);
    char path[600];
    snprintf(path, sizeof(path), IRDB_PACKED "/%s", rel);
    IrdbFile* e = add(path, &f);
    e->off = pw.off;
    e->len = len;
    irfile_free(&f);
    pw.off += len;
    return true;
}

bool irdb_pack_end(bool ok) {
    if(!pw.fp) ok = false;
    if(ok) {
        uint32_t toc = pw.off;
        for(uint32_t i = 0; i < pw.n; i++) {
            uint16_t pl = strlen(pw.rel[i]);
            fwrite(&pl, 2, 1, pw.fp);
            fwrite(pw.rel[i], 1, pl, pw.fp);
            fwrite(&pw.eoff[i], 4, 1, pw.fp);
            fwrite(&pw.elen[i], 4, 1, pw.fp);
        }
        fwrite(&pw.n, 4, 1, pw.fp);
        fwrite(&toc, 4, 1, pw.fp);
        fwrite(PACK_END, 1, 8, pw.fp);
        ok = !ferror(pw.fp);
    }
    if(pw.fp) ok &= !fclose(pw.fp);
    pw.fp = NULL;
    if(ok) {
        remove(ix.pack); // FAT's rename doesn't replace
        ok = !rename(pw.tmp, ix.pack);
    }
    if(ok) {
        set_version(pw.version);
        irdb_save();
    } else {
        remove(pw.tmp);
        irdb_unload(); // the cache still matches the old pack
    }
    for(uint32_t i = 0; i < pw.n; i++) free(pw.rel[i]);
    pw.n = 0;
    return ok;
}

// ---------------------------------------------------------------- the cache

// "3DSIRIX2", the pack's version (u8 length + text), u32 files, then each file: u16 path length, path,
// u32 offset and u32 length in the pack, u16 buttons, u8 library, u32 keys, keys (u8 proto, u8 button,
// u32 address, u32 command).
#define MAGIC "3DSIRIX2"
#define KEY_REC 10

static bool load_cache(void) {
    FILE* fp = fopen(ix.cache, "rb");
    if(!fp) return false;
    static char buf[32768];
    setvbuf(fp, buf, _IOFBF, sizeof(buf));
    char magic[8];
    uint8_t vl;
    uint32_t n;
    bool ok = get(fp, magic, 8) && !memcmp(magic, MAGIC, 8) && get(fp, &vl, 1) && vl < sizeof(ix.version) &&
              get(fp, ix.version, vl) && get(fp, &n, 4);
    if(ok) {
        ix.version[vl] = 0;
        ix.cap = n + 64;
        ix.f = calloc(ix.cap, sizeof(IrdbFile));
    }
    for(uint32_t i = 0; ok && i < n; i++) {
        IrdbFile* e = &ix.f[ix.n];
        uint16_t pl;
        uint8_t lib;
        ok = get(fp, &pl, 2) && (e->path = malloc(pl + 1)) && get(fp, e->path, pl) && get(fp, &e->off, 4) &&
             get(fp, &e->len, 4) && get(fp, &e->buttons, 2) && get(fp, &lib, 1) && get(fp, &e->nkeys, 4) &&
             e->nkeys <= 0xFFFF;
        if(!ok) {
            free(e->path);
            break;
        }
        e->path[pl] = 0;
        e->library = lib;
        e->keys = e->nkeys ? malloc(e->nkeys * sizeof(IrKey)) : NULL;
        // In one read: a file's keys field by field would be a million freads for the whole Flipper-IRDB.
        static uint8_t rec[0x10000 * KEY_REC];
        ok = get(fp, rec, e->nkeys * KEY_REC);
        for(uint32_t k = 0; ok && k < e->nkeys; k++) {
            const uint8_t* r = rec + k * KEY_REC;
            IrKey* key = &e->keys[k];
            key->proto = r[0];
            key->button = r[1];
            memcpy(&key->address, r + 2, 4);
            memcpy(&key->command, r + 6, 4);
        }
        ix.n++; // kept (freed with the rest) even if its keys came up short
        if(ix.n > 1 && strcmp(ix.f[ix.n - 2].path, e->path) >= 0) ok = false; // must be sorted
    }
    fclose(fp);
    if(!ok) clear();
    return ok;
}

bool irdb_save(void) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ix.cache);
    FILE* fp = fopen(tmp, "wb");
    if(!fp) return false;
    static char buf[32768];
    setvbuf(fp, buf, _IOFBF, sizeof(buf));
    uint8_t vl = strlen(ix.version);
    uint32_t n = ix.n;
    fwrite(MAGIC, 1, 8, fp);
    fwrite(&vl, 1, 1, fp);
    fwrite(ix.version, 1, vl, fp);
    fwrite(&n, 4, 1, fp);
    for(size_t i = 0; i < ix.n; i++) {
        const IrdbFile* e = &ix.f[i];
        uint16_t pl = strlen(e->path);
        uint8_t lib = e->library;
        fwrite(&pl, 2, 1, fp);
        fwrite(e->path, 1, pl, fp);
        fwrite(&e->off, 4, 1, fp);
        fwrite(&e->len, 4, 1, fp);
        fwrite(&e->buttons, 2, 1, fp);
        fwrite(&lib, 1, 1, fp);
        fwrite(&e->nkeys, 4, 1, fp);
        for(uint32_t k = 0; k < e->nkeys; k++) {
            const IrKey* key = &e->keys[k];
            uint8_t r[KEY_REC] = {key->proto, key->button};
            memcpy(r + 2, &key->address, 4);
            memcpy(r + 6, &key->command, 4);
            fwrite(r, 1, KEY_REC, fp);
        }
    }
    bool ok = !ferror(fp);
    ok &= !fclose(fp);
    if(ok) {
        remove(ix.cache); // FAT's rename doesn't replace
        ok = !rename(tmp, ix.cache);
    }
    if(!ok) remove(tmp);
    else ix.changed = false;
    return ok;
}

void irdb_touched(const char* path) {
    FILE* fp = fopen(ix.dirty, "a");
    if(!fp) return;
    fprintf(fp, "%s\n", path);
    fclose(fp);
}

// ---------------------------------------------------------------- walking the folder

typedef struct {
    char** p;
    size_t n, cap;
} Paths;

static void paths_add(Paths* ps, const char* s) {
    if(ps->n == ps->cap) ps->p = realloc(ps->p, (ps->cap = ps->cap ? ps->cap * 2 : 256) * sizeof(char*));
    ps->p[ps->n++] = strdup(s);
}

static void paths_free(Paths* ps) {
    for(size_t i = 0; i < ps->n; i++) free(ps->p[i]);
    free(ps->p);
    *ps = (Paths){0};
}

static int cmp_path(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static bool is_ir(const char* name) {
    size_t n = strlen(name);
    return n > 3 && !strcasecmp(name + n - 3, ".ir");
}

// Every .ir file below dir (dot files and folders skipped).
static void walk(const char* dir, Paths* out, volatile bool* cancel) {
    DIR* d = opendir(dir);
    if(!d) return;
    struct dirent* e;
    char path[512];
    while((e = readdir(d)) && !(cancel && *cancel)) {
        if(e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        bool isdir = e->d_type == DT_DIR;
        if(e->d_type == DT_UNKNOWN) {
            struct stat st;
            isdir = !stat(path, &st) && S_ISDIR(st.st_mode);
        }
        if(isdir) walk(path, out, cancel);
        else if(is_ir(e->d_name)) paths_add(out, path);
    }
    closedir(d);
}

static bool update(bool all, volatile bool* cancel, void (*progress)(size_t, size_t)) {
    if(!ix.loaded || all) {
        clear();
        if(!all) load_cache();
        ix.loaded = true;
        ix.changed = all;
    }
    // The pack, if it isn't the one indexed.
    char v[PACK_VERSION + 1];
    irdb_pack_version(v, sizeof(v));
    bool done = true;
    if(all || strcmp(v, ix.version)) done = index_pack(cancel, progress);

    Paths now = {0}, todo = {0};
    if(done) walk(ix.root, &now, cancel);
    if(!done || (cancel && *cancel)) {
        paths_free(&now);
        if(ix.changed) irdb_save();
        return false;
    }
    if(now.n) qsort(now.p, now.n, sizeof(char*), cmp_path);
    // Gone: indexed, not found.
    for(size_t i = ix.n; i-- > 0;) {
        if(irdb_is_packed(ix.f[i].path)) continue;
        if(!bsearch(&ix.f[i].path, now.p, now.n, sizeof(char*), cmp_path)) remove_at(i);
    }
    // New, and what the app changed.
    for(size_t i = 0; i < now.n; i++) {
        bool found;
        find(now.p[i], &found);
        if(!found) paths_add(&todo, now.p[i]);
    }
    FILE* fp = fopen(ix.dirty, "r");
    if(fp) {
        char line[512];
        while(fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if(!*line || irdb_is_packed(line)) continue;
            bool found;
            size_t at = find(line, &found);
            if(found) remove_at(at);
            if(bsearch(&(char*){line}, now.p, now.n, sizeof(char*), cmp_path)) paths_add(&todo, line);
        }
        fclose(fp);
    }
    paths_free(&now);

    for(size_t i = 0; i < todo.n; i++) {
        if(cancel && *cancel) {
            done = false;
            break;
        }
        if(progress) progress(i, todo.n);
        IrFile f;
        if(irfile_load(todo.p[i], &f)) {
            irdb_add(todo.p[i], &f);
            irfile_free(&f);
        }
    }
    paths_free(&todo);
    bool saved = !ix.changed || irdb_save();
    if(saved && done) remove(ix.dirty); // else they're looked at again next time
    return done;
}

void irdb_open(void) {
    if(ix.loaded) return;
    clear();
    load_cache();
    ix.loaded = true;
    ix.changed = false;
}

void irdb_unload(void) {
    clear();
    ix.loaded = false;
}

bool irdb_refresh(volatile bool* cancel, void (*progress)(size_t, size_t)) {
    return update(false, cancel, progress);
}

bool irdb_rebuild(volatile bool* cancel, void (*progress)(size_t, size_t)) {
    return update(true, cancel, progress);
}

// ---------------------------------------------------------------- finding a remote

#define MAX_PRESSES 32

static struct {
    IrKey press[MAX_PRESSES];
    int asked[MAX_PRESSES];
    size_t presses;
    bool skipped[64]; // by button id: skipped or asked already
    const IrdbFile** cand;
    size_t n;
} fd;

static bool has(const IrdbFile* f, const IrKey* k) {
    for(uint32_t i = 0; i < f->nkeys; i++)
        if(irdb_same_code(&f->keys[i], k)) return true;
    return false;
}

// The first code a file has for a button.
static const IrKey* code_for(const IrdbFile* f, int button) {
    for(uint32_t i = 0; i < f->nkeys; i++)
        if(f->keys[i].button == button) return &f->keys[i];
    return NULL;
}

// The file has the button, and none of its codes for it is k.
static bool named_other(const IrdbFile* f, const IrKey* k, int button) {
    bool named = false;
    for(uint32_t i = 0; i < f->nkeys; i++) {
        if(f->keys[i].button != button) continue;
        if(irdb_same_code(&f->keys[i], k)) return false;
        named = true;
    }
    return named;
}

// How many presses a file has under the very button they were asked for.
static int named_hits(const IrdbFile* f) {
    int k = 0;
    for(size_t i = 0; i < fd.presses; i++) {
        if(fd.asked[i] == IRDB_BTN_OTHER) continue;
        const IrKey* c = code_for(f, fd.asked[i]);
        k += c && irdb_same_code(c, &fd.press[i]);
    }
    return k;
}

static int cmp_cand(const void* a, const void* b) {
    const IrdbFile *x = *(const IrdbFile* const*)a, *y = *(const IrdbFile* const*)b;
    int hx = named_hits(x), hy = named_hits(y);
    if(hx != hy) return hy - hx;
    if(x->buttons != y->buttons) return (int)y->buttons - (int)x->buttons;
    return strcmp(x->path, y->path);
}

void irdb_find_reset(void) {
    free(fd.cand);
    memset(&fd, 0, sizeof(fd));
}

bool irdb_find_press(const IrKey* k, int asked) {
    if(fd.presses == MAX_PRESSES) return false;
    const IrdbFile** next = malloc((fd.presses ? fd.n : ix.n) * sizeof(*next) + 1);
    size_t m = 0;
    if(!fd.presses) {
        for(size_t i = 0; i < ix.n; i++)
            if(!ix.f[i].library && has(&ix.f[i], k)) next[m++] = &ix.f[i];
    } else {
        for(size_t i = 0; i < fd.n; i++)
            if(has(fd.cand[i], k)) next[m++] = fd.cand[i];
    }
    if(asked >= 0 && asked < 64) fd.skipped[asked] = true;
    // Asked for a button: a remote whose own button of that name sends something else isn't it (unless that
    // would leave none: names in the database aren't always right).
    if(m && asked != IRDB_BTN_OTHER) {
        size_t keep = 0;
        for(size_t i = 0; i < m; i++) keep += !named_other(next[i], k, asked);
        if(keep) {
            size_t j = 0;
            for(size_t i = 0; i < m; i++)
                if(!named_other(next[i], k, asked)) next[j++] = next[i];
            m = j;
        }
    }
    if(!m) {
        free(next);
        return false;
    }
    free(fd.cand);
    fd.cand = next;
    fd.n = m;
    fd.press[fd.presses] = *k;
    fd.asked[fd.presses++] = asked;
    qsort(fd.cand, fd.n, sizeof(*fd.cand), cmp_cand);
    return true;
}

void irdb_find_skip(int button) {
    if(button < 0 || button >= 64) return;
    fd.skipped[button] = true;
    // The remote has no such button: the remotes that do aren't it (unless that would leave none). A remote
    // without Down has no Up either, and isn't asked for it.
    size_t keep = 0;
    for(size_t i = 0; i < fd.n; i++) keep += !code_for(fd.cand[i], button);
    if(!keep || keep == fd.n) return;
    size_t j = 0;
    for(size_t i = 0; i < fd.n; i++)
        if(!code_for(fd.cand[i], button)) fd.cand[j++] = fd.cand[i];
    fd.n = j;
}

size_t irdb_find_presses(void) {
    return fd.presses;
}

size_t irdb_find_candidates(const IrdbFile* const** out) {
    *out = fd.cand;
    return fd.n;
}

static int cmp_key(const void* a, const void* b) {
    const IrKey *x = *(const IrKey* const*)a, *y = *(const IrKey* const*)b;
    if(x->proto != y->proto) return x->proto - y->proto;
    if(x->address != y->address) return x->address < y->address ? -1 : 1;
    if(x->command != y->command) return x->command < y->command ? -1 : 1;
    return 0;
}

int irdb_find_next(void) {
    if(fd.n < 2) return -1;
    const IrKey** codes = malloc(fd.n * sizeof(*codes));
    int best = -1;
    // Worth a press only if it leaves 15% fewer on average: past that, what's left is mostly the same remote
    // filed again (tests/irdb_test.c on the Flipper-IRDB: 3.2 presses on average, and 0.95 would take 3.8
    // to tell apart only 2% more).
    double best_e = fd.n * 0.85;
    if(best_e > fd.n - 0.5) best_e = fd.n - 0.5;
    for(int b = 0; b < NBUTTONS; b++) {
        if(fd.skipped[b]) continue;
        // If the remote is candidate i, pressing b leaves those with i's code for b; if it hasn't one
        // (as far as the database knows), nothing is learned.
        size_t m = 0, none = 0;
        for(size_t i = 0; i < fd.n; i++) {
            const IrKey* c = code_for(fd.cand[i], b);
            if(c) codes[m++] = c;
            else none++;
        }
        qsort(codes, m, sizeof(*codes), cmp_key);
        double sq = 0;
        for(size_t i = 0; i < m;) {
            size_t j = i;
            while(j < m && !cmp_key(&codes[i], &codes[j])) j++;
            sq += (double)(j - i) * (j - i);
            i = j;
        }
        double e = (sq + (double)none * fd.n) / fd.n;
        if(e < best_e) {
            best_e = e;
            best = b;
        }
    }
    free(codes);
    return best;
}
