#pragma once
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>

// Log levels
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

// Global log level (can be set via command line)
inline LogLevel g_log_level = LogLevel::INFO;

// ANSI color codes for terminal output
inline const char* log_level_color(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return "\033[36m";  // cyan
    case LogLevel::INFO:  return "\033[32m";  // green
    case LogLevel::WARN:  return "\033[33m";  // yellow
    case LogLevel::ERROR: return "\033[31m";  // red
    }
    return "";
}

inline const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    case LogLevel::ERROR: return "ERROR";
    }
    return "?";
}

inline void log_message(LogLevel level, const char* fmt, ...) {
    if (level < g_log_level) return;

    // Get timestamp
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    // Print log prefix with color
    fprintf(stderr, "%s[%s] [%s]\033[0m ",
            log_level_color(level), time_buf, log_level_name(level));

    // Print message
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

// Convenience macros
#define LOG_DEBUG(...) log_message(LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_message(LogLevel::INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_message(LogLevel::WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_message(LogLevel::ERROR, __VA_ARGS__)