#pragma once
#include <format>
#include <iostream>

enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };
inline LogLevel g_log_level = LogLevel::Info;

inline const char* prefix(const LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "[TRACE] ";
        case LogLevel::Debug: return "[DEBUG] ";
        case LogLevel::Info:  return "[INFO]  ";
        case LogLevel::Warn:  return "[WARN]  ";
        case LogLevel::Error: return "[ERROR] ";
        default:              return "";
    }
}

template <typename... Args>
void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < g_log_level) return;
    std::cerr << prefix(level)
              << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

// Parse a level name into g_log_level. Returns false on an unknown name so
// the caller can report a usage error.
inline bool set_log_level(const std::string_view s) {
    if      (s == "trace") g_log_level = LogLevel::Trace;
    else if (s == "debug") g_log_level = LogLevel::Debug;
    else if (s == "info")  g_log_level = LogLevel::Info;
    else if (s == "warn")  g_log_level = LogLevel::Warn;
    else if (s == "error") g_log_level = LogLevel::Error;
    else if (s == "off")   g_log_level = LogLevel::Off;
    else return false;
    return true;
}