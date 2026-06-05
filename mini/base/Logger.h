#pragma once

// Logger 提供轻量级日志能力，包含五个级别 (TRACE/DEBUG/INFO/WARN/ERROR/FATAL)。
// FATAL 级别在输出后调用 std::abort()。
// 使用 LOG_xxx 宏自动附加文件名和行号。
// 用户可通过 Logger::setLogLevel() 控制输出级别。
// 用户可通过 Logger::setOutputFunction() 替换输出目标（默认 stderr）。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace mini::base {

class Logger {
public:
    enum LogLevel {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR,
        FATAL,
        NUM_LOG_LEVELS,
    };

    using OutputFunc = std::function<void(const char* msg, int len)>;
    using FlushFunc = std::function<void()>;

    Logger(const char* file, int line, LogLevel level);
    ~Logger();

    Logger& stream() { return *this; }

    Logger& operator<<(const char* str);
    Logger& operator<<(const std::string& str);
    Logger& operator<<(std::string_view str);
    Logger& operator<<(int val);
    Logger& operator<<(unsigned int val);
    Logger& operator<<(long val);
    Logger& operator<<(unsigned long val);
    Logger& operator<<(long long val);
    Logger& operator<<(double val);
    Logger& operator<<(char c);

    static LogLevel logLevel();
    static void setLogLevel(LogLevel level);
    static void setOutputFunction(OutputFunc func);
    static void setFlushFunction(FlushFunc func);

private:
    void formatHeader();

    static const char* levelName(LogLevel level);
    static const char* extractFileName(const char* path);

    const char* file_;
    int line_;
    LogLevel level_;
    std::string buffer_;
};

// Global log level (for macro fast-path check)
Logger::LogLevel logLevel();

}  // namespace mini::base

// ── Convenience macros ──
// Each macro checks the current log level before constructing a Logger object,
// avoiding any formatting overhead when the level is suppressed.

#define LOG_TRACE                                                          \
    if (mini::base::logLevel() <= mini::base::Logger::TRACE)              \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::TRACE).stream()

#define LOG_DEBUG                                                          \
    if (mini::base::logLevel() <= mini::base::Logger::DEBUG)              \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::DEBUG).stream()

#define LOG_INFO                                                           \
    if (mini::base::logLevel() <= mini::base::Logger::INFO)               \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::INFO).stream()

#define LOG_WARN                                                           \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::WARN).stream()

#define LOG_ERROR                                                          \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::ERROR).stream()

#define LOG_FATAL                                                          \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::FATAL).stream()

#define LOG_SYSERR                                                         \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::ERROR).stream()

#define LOG_SYSFATAL                                                       \
    mini::base::Logger(__FILE__, __LINE__, mini::base::Logger::FATAL).stream()
