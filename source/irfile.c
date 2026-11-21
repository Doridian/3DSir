#include "irfile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* trim(char* s) {
    while(isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while(e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// "04 00 00 00" -> 0x00000004 (little endian byte list)
static uint32_t parse_le_hex(const char* s) {
    uint32_t v = 0;
    for(int i = 0; i < 4; i++) {
        char* end;
        unsigned long b = strtoul(s, &end, 16);
        if(end == s) break;
        v |= (uint32_t)(b & 0xFF) << (8 * i);
        s = end;
    }
    return v;
}

static void parse_data(IrSignal* sig, const char* s) {
    size_t cap = 64;
    sig->data = malloc(cap * sizeof(uint32_t));
    sig->data_n = 0;
    for(;;) {
        char* end;
        unsigned long v = strtoul(s, &end, 10);
        if(end == s) break;
        if(sig->data_n == cap) {
            cap *= 2;
            sig->data = realloc(sig->data, cap * sizeof(uint32_t));
        }
        sig->data[sig->data_n++] = v;
        s = end;
    }
}

void irfile_init(IrFile* f) {
    memset(f, 0, sizeof(*f));
    snprintf(f->filetype, sizeof(f->filetype), "IR signals file");
}

bool irfile_load(const char* path, IrFile* out) {
    FILE* fp = fopen(path, "r");
    if(!fp) return false;

    irfile_init(out);
    size_t cap = 0;
    IrSignal* cur = NULL;
    static char line[16384];

    while(fgets(line, sizeof(line), fp)) {
        char* colon = strchr(line, ':');
        if(line[0] == '#' || !colon) continue;
        *colon = 0;
        char* key = trim(line);
        char* val = trim(colon + 1);

        if(!strcmp(key, "Filetype")) {
            snprintf(out->filetype, sizeof(out->filetype), "%s", val);
        } else if(!strcmp(key, "name")) {
            if(out->n == cap) {
                cap = cap ? cap * 2 : 32;
                out->sig = realloc(out->sig, cap * sizeof(IrSignal));
            }
            cur = &out->sig[out->n++];
            memset(cur, 0, sizeof(*cur));
            snprintf(cur->name, sizeof(cur->name), "%s", val);
        } else if(!cur) {
            continue;
        } else if(!strcmp(key, "type")) {
            cur->raw = !strcmp(val, "raw");
        } else if(!strcmp(key, "protocol")) {
            snprintf(cur->protocol, sizeof(cur->protocol), "%s", val);
        } else if(!strcmp(key, "address")) {
            cur->address = parse_le_hex(val);
        } else if(!strcmp(key, "command")) {
            cur->command = parse_le_hex(val);
        } else if(!strcmp(key, "frequency")) {
            cur->frequency = strtoul(val, NULL, 10);
        } else if(!strcmp(key, "duty_cycle")) {
            snprintf(cur->duty, sizeof(cur->duty), "%s", val);
        } else if(!strcmp(key, "data")) {
            free(cur->data);
            parse_data(cur, val);
        }
    }
    fclose(fp);
    return true;
}

static void write_le_hex(FILE* fp, const char* key, uint32_t v) {
    fprintf(fp, "%s: %02X %02X %02X %02X\n", key, (unsigned)(v & 0xFF), (unsigned)(v >> 8 & 0xFF),
            (unsigned)(v >> 16 & 0xFF), (unsigned)(v >> 24));
}

bool irfile_save(const IrFile* f, const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* fp = fopen(tmp, "w");
    if(!fp) return false;
    static char buf[8192];
    setvbuf(fp, buf, _IOFBF, sizeof(buf));
    fprintf(fp, "Filetype: %s\nVersion: 1\n", *f->filetype ? f->filetype : "IR signals file");
    for(size_t i = 0; i < f->n; i++) {
        const IrSignal* s = &f->sig[i];
        fprintf(fp, "# \nname: %s\n", s->name);
        if(s->raw) {
            fprintf(fp, "type: raw\nfrequency: %lu\nduty_cycle: %s\ndata:", (unsigned long)s->frequency,
                    *s->duty ? s->duty : "0.330000");
            for(size_t k = 0; k < s->data_n; k++) fprintf(fp, " %lu", (unsigned long)s->data[k]);
            fputc('\n', fp);
        } else {
            fprintf(fp, "type: parsed\nprotocol: %s\n", s->protocol);
            write_le_hex(fp, "address", s->address);
            write_le_hex(fp, "command", s->command);
        }
    }
    bool ok = !ferror(fp);
    ok &= !fclose(fp);
    if(!ok) {
        remove(tmp);
        return false;
    }
    remove(path); // FAT's rename doesn't replace
    return !rename(tmp, path);
}

static void copy_code(IrSignal* d, const IrSignal* s) {
    free(d->data);
    char name[sizeof(d->name)];
    memcpy(name, d->name, sizeof(name));
    *d = *s;
    memcpy(d->name, name, sizeof(name));
    d->data = NULL;
    if(s->raw && s->data_n) {
        d->data = malloc(s->data_n * sizeof(uint32_t));
        memcpy(d->data, s->data, s->data_n * sizeof(uint32_t));
    }
}

void irfile_add(IrFile* f, const IrSignal* s) {
    f->sig = realloc(f->sig, (f->n + 1) * sizeof(IrSignal));
    IrSignal* d = &f->sig[f->n++];
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    copy_code(d, s);
}

void irfile_remove(IrFile* f, size_t i) {
    if(i >= f->n) return;
    free(f->sig[i].data);
    memmove(&f->sig[i], &f->sig[i + 1], (f->n - i - 1) * sizeof(IrSignal));
    f->n--;
}

void irfile_move(IrFile* f, size_t from, size_t to) {
    if(from >= f->n || to >= f->n || from == to) return;
    IrSignal s = f->sig[from];
    if(from < to) memmove(&f->sig[from], &f->sig[from + 1], (to - from) * sizeof(IrSignal));
    else memmove(&f->sig[to + 1], &f->sig[to], (from - to) * sizeof(IrSignal));
    f->sig[to] = s;
}

void irfile_replace_code(IrFile* f, size_t i, const IrSignal* s) {
    if(i < f->n) copy_code(&f->sig[i], s);
}

void irfile_describe(const IrSignal* s, char* out, size_t max) {
    if(s->raw)
        snprintf(out, max, "raw, %lu kHz, %zu samples", (unsigned long)(s->frequency + 500) / 1000, s->data_n);
    else
        snprintf(out, max, "%s  %02lX / %02lX", s->protocol, (unsigned long)s->address, (unsigned long)s->command);
}

void irfile_free(IrFile* f) {
    for(size_t i = 0; i < f->n; i++) free(f->sig[i].data);
    free(f->sig);
    f->sig = NULL;
    f->n = 0;
}

// Gap between repeats when the signal does not carry its own trailing space.
#define DEFAULT_REPEAT_GAP_US 40000

bool irfile_signal_timings(const IrSignal* s, unsigned repeats, IrTimings* out) {
    uint32_t gap = DEFAULT_REPEAT_GAP_US;
    if(!s->raw) {
        if(!ir_encode_parsed(s->protocol, s->address, s->command, out)) return false;
    } else {
        ir_timings_clear(out);
        if(!s->data_n) return false;
        out->freq = s->frequency;
        for(size_t i = 0; i < s->data_n; i++) {
            if(i % 2 == 0)
                ir_timings_mark(out, s->data[i]);
            else
                ir_timings_space(out, s->data[i]);
        }
        // A raw capture's trailing space is the remote's own repeat gap.
        if(out->n % 2 == 0) gap = out->t[--out->n];
    }
    ir_timings_repeat(out, repeats, gap);
    return true;
}
