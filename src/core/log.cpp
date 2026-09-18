#include "log.h"

#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>

namespace wr {
namespace {

LogLevel g_level = LogLevel::Info;
std::string g_tail;
std::mutex g_mutex;
std::vector<LogSink> g_sinks;
int g_errors = 0;

const char *level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// Just the file name; the path is noise in a log line.
const char *short_file(const char *path) {
    const char *slash = std::strrchr(path, '/');
#ifdef _WIN32
    const char *back = std::strrchr(path, '\\');
    if (back > slash) slash = back;
#endif
    return slash ? slash + 1 : path;
}

}  // namespace

void log_set_level(LogLevel l) { g_level = l; }
LogLevel log_get_level() { return g_level; }
int error_count() { return g_errors; }

void log_write(LogLevel l, const char *file, int line, const char *fmt, ...) {
    if (l < g_level) return;
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char line_buf[2304];
    if (l >= LogLevel::Warn)
        std::snprintf(line_buf, sizeof(line_buf), "[%s] %s  (%s:%d)\n",
                      level_name(l), body, short_file(file), line);
    else
        std::snprintf(line_buf, sizeof(line_buf), "[%s] %s\n", level_name(l), body);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (l >= LogLevel::Error) g_errors++;
    std::fputs(line_buf, l >= LogLevel::Warn ? stderr : stdout);
    if (l >= LogLevel::Warn) std::fflush(stderr);
    g_tail += line_buf;
    // The sinks get the body without the level prefix or the newline:
    // a console has its own idea of how to show a warning, and
    // re-parsing what was just formatted would be silly.
    for (LogSink sink : g_sinks)
        if (sink) sink(l, body);
    // Keep the tail bounded; a render loop can produce a lot of it.
    if (g_tail.size() > 96 * 1024)
        g_tail.erase(0, g_tail.size() - 64 * 1024);
}

const std::string &log_tail() { return g_tail; }

void log_add_sink(LogSink sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (LogSink s : g_sinks)
        if (s == sink) return;
    g_sinks.push_back(sink);
}

void log_remove_sink(LogSink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sinks.erase(std::remove(g_sinks.begin(), g_sinks.end(), sink),
                  g_sinks.end());
}

}  // namespace wr
