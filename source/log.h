#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// The app log: a ring of the last lines (the log screen, the bottom screen, the debug build's net
// server) and, in the debug build, sdmc:/3dsir.log. Any thread.
void log_init(const char* file_path);
void log_exit(void);
void logp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

int log_count(void);
void log_line(int i, char* out, size_t max); // i from 0 (oldest kept) to log_count() - 1
