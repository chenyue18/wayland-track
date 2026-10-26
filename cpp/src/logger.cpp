#include "logger.hpp"

#include <cstdarg>

#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include "util.hpp"

static FILE* g_log = nullptr;

void set_log_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    FILE* f = fdopen(fd, "a");
    if (f) g_log = f;
}

void logf(const char* f, ...) {
    FILE* out = g_log ? g_log : stderr;
    std::time_t tt = std::time(nullptr);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);
    va_list ap;
    va_start(ap, f);
    fprintf(out, "wayland-track: %s ", tbuf);
    vfprintf(out, f, ap);
    fputc('\n', out);
    va_end(ap);
    fflush(out);
}
