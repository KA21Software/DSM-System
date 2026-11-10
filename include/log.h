// Per-node logger. Thread-safe (one mutex). NOT async-signal-safe: never call it from the
// SIGSEGV handler (it takes a mutex and uses stdio).
#pragma once
#include <string>

enum LogLevel { LOG_LVL_ERROR = 0, LOG_LVL_WARN = 1, LOG_LVL_INFO = 2, LOG_LVL_DEBUG = 3 };

// Opens <dir>/node-<tag>.log (creating dir). `level` is ERROR, WARN, INFO or DEBUG.
// Returns false if the level is unknown or the file cannot be opened.
bool log_init(const std::string& dir, const std::string& level, const std::string& tag);
void log_close();
bool log_enabled(LogLevel lvl);
void log_write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_ERROR(...) log_write(LOG_LVL_ERROR, __VA_ARGS__)
#define LOG_WARN(...) log_write(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_INFO(...) log_write(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) log_write(LOG_LVL_DEBUG, __VA_ARGS__)
