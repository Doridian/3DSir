#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RING_LINES 256
#define LINE_MAX 128

static PrintConsole* con;
static FILE* file;
static LightLock lock;
static char ring[RING_LINES][LINE_MAX];
static int ring_head, ring_n; // next slot to fill, lines kept
static char partial[LINE_MAX]; // the current line until its '\n'
static size_t partial_len;

void log_init(PrintConsole* console, const char* file_path) {
    LightLock_Init(&lock);
    con = console;
    if(file_path) file = fopen(file_path, "a");
}

void log_exit(void) {
    if(file) fclose(file);
    file = NULL;
}

static void push_line(void) {
    partial[partial_len] = 0;
    memcpy(ring[ring_head], partial, partial_len + 1);
    ring_head = (ring_head + 1) % RING_LINES;
    if(ring_n < RING_LINES) ring_n++;
    partial_len = 0;
}

void logp(const char* fmt, ...) {
    char s[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(s, sizeof(s), fmt, va);
    va_end(va);

    // The console isn't thread safe: only the main thread (no libctru Thread) prints there.
    if(con && !threadGetCurrent()) {
        PrintConsole* prev = consoleSelect(con);
        fputs(s, stdout);
        consoleSelect(prev);
    }

    // The ring and the file get the text without its ANSI color codes.
    char plain[512];
    size_t n = 0;
    for(const char* p = s; *p; p++) {
        if(*p == '\x1b' && p[1] == '[') {
            p += 2;
            while(*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
            if(!*p) break;
            continue;
        }
        plain[n++] = *p;
    }
    plain[n] = 0;

    LightLock_Lock(&lock);
    for(size_t i = 0; i < n; i++) {
        if(plain[i] == '\n')
            push_line();
        else if(partial_len < LINE_MAX - 1)
            partial[partial_len++] = plain[i];
    }
    if(file) {
        fputs(plain, file);
        fflush(file);
    }
    LightLock_Unlock(&lock);
}

int log_count(void) {
    return ring_n;
}

void log_line(int i, char* out, size_t max) {
    LightLock_Lock(&lock);
    if(i < 0 || i >= ring_n) {
        out[0] = 0;
    } else {
        int idx = (ring_head - ring_n + i + RING_LINES) % RING_LINES;
        snprintf(out, max, "%s", ring[idx]);
    }
    LightLock_Unlock(&lock);
}
