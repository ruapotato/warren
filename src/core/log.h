// Manifold -- saying what happened.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace mf {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Fatal };

void log_set_level(LogLevel l);
LogLevel log_get_level();
void log_write(LogLevel l, const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
// Every message ever written, for a debug overlay or a crash report.
const std::string &log_tail();

// ANOTHER PLACE FOR MESSAGES TO GO.
//
// The editor's console is the reason this exists: a warning from the
// renderer should appear where the person using the engine is
// looking, not only in a terminal behind the window. A sink is
// called with the message body, already formatted and without the
// trailing newline, while the log's own mutex is held -- so it must
// not log, and must not block.
using LogSink = void (*)(LogLevel, const char *);
void log_add_sink(LogSink sink);
void log_remove_sink(LogSink sink);

// An error that should stop the frame but not the process. Returns the
// count so a caller can tell whether anything went wrong in a block.
int error_count();

}  // namespace mf

#define MF_TRACE(...) ::mf::log_write(::mf::LogLevel::Trace, __FILE__, __LINE__, __VA_ARGS__)
#define MF_DEBUG(...) ::mf::log_write(::mf::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define MF_INFO(...)  ::mf::log_write(::mf::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define MF_WARN(...)  ::mf::log_write(::mf::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define MF_ERROR(...) ::mf::log_write(::mf::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
#define MF_FATAL(...) ::mf::log_write(::mf::LogLevel::Fatal, __FILE__, __LINE__, __VA_ARGS__)

// Check a precondition and return if it fails, saying so once.
#define MF_CHECK(cond, ...)                   \
    do {                                      \
        if (!(cond)) {                        \
            MF_ERROR(__VA_ARGS__);            \
            return;                           \
        }                                     \
    } while (0)
#define MF_CHECK_V(cond, ret, ...)            \
    do {                                      \
        if (!(cond)) {                        \
            MF_ERROR(__VA_ARGS__);            \
            return ret;                       \
        }                                     \
    } while (0)
