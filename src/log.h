#pragma once
#include <cstdarg>
#include <cstdio>

namespace xl {

inline bool& verbose_logging() {
    static bool v = false;
    return v;
}

inline void log_line(FILE* out, const char* prefix, const char* fmt, va_list args) {
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
