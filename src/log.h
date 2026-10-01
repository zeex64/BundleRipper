#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>

namespace xl {

inline bool& verbose_logging() {
    static bool v = false;
    return v;
}

// When set, log lines go here instead of stdout (the GUI's log panel). Called from any thread.
using LogSink = void (*)(const std::string& line);
inline LogSink& log_sink() {
    static LogSink s = nullptr;
    return s;
}

inline void log_line(FILE* out, const char* prefix, const char* fmt, va_list args) {
    if (LogSink sink = log_sink()) {
        va_list copy;
        va_copy(copy, args);
        int n = std::vsnprintf(nullptr, 0, fmt, copy);
        va_end(copy);
        std::string line = prefix;
        if (n > 0) {
            size_t at = line.size();
            line.resize(at + (size_t)n + 1);
            std::vsnprintf(line.data() + at, (size_t)n + 1, fmt, args);
            line.resize(at + (size_t)n);
        }
        sink(line);
        return;
    }
    std::fputs(prefix, out);
    std::vfprintf(out, fmt, args);
    std::fputc('\n', out);
    std::fflush(out);
}

inline void log_info(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    log_line(stdout, "", fmt, a);
    va_end(a);
}

inline void log_verbose(const char* fmt, ...) {
    if (!verbose_logging()) return;
    va_list a;
    va_start(a, fmt);
    log_line(stdout, "  ", fmt, a);
    va_end(a);
}

inline void log_warn(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    log_line(stdout, "warning: ", fmt, a);
    va_end(a);
}

} // namespace xl
