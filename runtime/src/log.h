// Logging: LOG always, TRACE when --trace is on. Output goes to stderr and the log file.
#pragma once
#include <stdint.h>

extern int g_log_level;  // 0 normal, 1+ trace
void log_set_file(const char* path);
// Minimal signal-safe record: no heap, stdio, locks, or stack unwinding.
void log_emergency(int signal, uintptr_t address);
void log_write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) log_write(__VA_ARGS__)
#define TRACE(...) do { if (g_log_level > 0) log_write(__VA_ARGS__); } while (0)
