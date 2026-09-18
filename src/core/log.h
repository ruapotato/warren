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
