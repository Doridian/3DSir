#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// The app log: the bottom screen (when called from the main thread), a ring of the
// last lines (served to the PC by the debug build's net server) and, in the debug
// build, sdmc:/3dsir.log.
void log_init(PrintConsole* console, const char* file_path);
void log_exit(void);
void logp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

int log_count(void);
void log_line(int i, char* out, size_t max); // i from 0 (oldest kept) to log_count() - 1
