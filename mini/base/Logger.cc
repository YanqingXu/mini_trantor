#include "mini/base/Logger.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace mini::base {

namespace {

Logger::LogLevel g_logLevel = Logger::INFO;
Logger::OutputFunc g_outputFunc = nullptr;
Logger::FlushFunc g_flushFunc = nullptr;

FILE* g_logFile = nullptr;

FILE* getLogFile() {
    if (!g_logFile) {
        g_logFile = std::fopen("log", "a");
        if (!g_logFile) {
            g_logFile = stderr;
        }
    }
    return g_logFile;
}

void defaultOutput(const char* msg, int len) {
    std::fwrite(msg, 1, static_cast<std::size_t>(len), getLogFile());
}

void defaultFlush() {
    std::fflush(getLogFile());
}

}  // namespace

const char* Logger::levelName(LogLevel level) {
    static const char* names[NUM_LOG_LEVELS] = {
        "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL",
    };
    return names[level];
}

const char* Logger::extractFileName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

Logger::Logger(const char* file, int line, LogLevel level)
    : file_(extractFileName(file)), line_(line), level_(level) {
    formatHeader();
}

Logger::~Logger() {
    buffer_ += '\n';

    auto output = g_outputFunc ? g_outputFunc : defaultOutput;
    output(buffer_.data(), static_cast<int>(buffer_.size()));

    if (level_ == FATAL) {
        auto flush = g_flushFunc ? g_flushFunc : defaultFlush;
        flush();
        std::abort();
    }
}

void Logger::formatHeader() {
    // Format: "20260406 14:30:15.123456 LEVEL file.cc:42 - "
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);
    const auto us = duration_cast<microseconds>(now.time_since_epoch()) % 1000000;

    struct tm tm{};
    ::localtime_r(&tt, &tm);

    char timeBuf[32];
    const int timeLen = std::snprintf(timeBuf, sizeof(timeBuf),
        "%04d%02d%02d %02d:%02d:%02d.%06ld",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long>(us.count()));

    buffer_.reserve(128);
    buffer_.append(timeBuf, static_cast<std::size_t>(timeLen));
    buffer_ += ' ';
    buffer_.append(levelName(level_), 5);
    buffer_ += ' ';
    buffer_.append(file_);
    buffer_ += ':';

    char lineBuf[16];
    const int lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%d", line_);
    buffer_.append(lineBuf, static_cast<std::size_t>(lineLen));

    buffer_.append(" - ", 3);
}

Logger& Logger::operator<<(const char* str) {
    buffer_.append(str ? str : "(null)");
    return *this;
}

Logger& Logger::operator<<(const std::string& str) {
    buffer_.append(str);
    return *this;
}

Logger& Logger::operator<<(std::string_view str) {
    buffer_.append(str);
    return *this;
}

Logger& Logger::operator<<(int val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(unsigned int val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(unsigned long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(long long val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(double val) {
    buffer_.append(std::to_string(val));
    return *this;
}

Logger& Logger::operator<<(char c) {
    buffer_ += c;
    return *this;
}

Logger::LogLevel Logger::logLevel() {
    return g_logLevel;
}

void Logger::setLogLevel(LogLevel level) {
    g_logLevel = level;
}

void Logger::setOutputFunction(OutputFunc func) {
    g_outputFunc = std::move(func);
}

void Logger::setFlushFunction(FlushFunc func) {
    g_flushFunc = std::move(func);
}

Logger::LogLevel logLevel() {
    return Logger::logLevel();
}

}  // namespace mini::base
