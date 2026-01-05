#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef _WIN32
#include <syslog.h>
#endif

// Logger/formatting utilities intentionally use macros and low-level buffer manipulation for speed.
// Suppress noisy clang-tidy checks in this header to keep analysis actionable.
// NOLINTBEGIN

namespace spaznet {

// Log levels - higher value = more verbose
enum class LogLevel : uint8_t { NONE = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4, TRACE = 5 };

// Forward declarations
class Logger;

// Output backends
enum class LogBackend { STDOUT, SYSLOG };

// Format types
enum class LogFormat {
    TEXT,  // Simple text format
    JSON,  // JSON format
    SYSLOG // RFC 3164/5424 syslog format
};

// Compile-time gate - only enabled if DEBUG or TRACE level is enabled
#ifdef NDEBUG
#ifndef ENABLE_TRACE_LOGS
#define ENABLE_TRACE_LOGS 0
#endif
#ifndef ENABLE_DEBUG_LOGS
#define ENABLE_DEBUG_LOGS 0
#endif
#else
#ifndef ENABLE_TRACE_LOGS
#define ENABLE_TRACE_LOGS 1
#endif
#ifndef ENABLE_DEBUG_LOGS
#define ENABLE_DEBUG_LOGS 1
#endif
#endif

// Internal namespace for implementation details
namespace detail {

// Timestamp formatting buffer size
constexpr size_t TIMESTAMP_BUFFER_SIZE = 64;
constexpr size_t MAX_LOG_MESSAGE_SIZE = 4096;

// Thread-local timestamp buffer to avoid allocations
thread_local char timestamp_buffer[TIMESTAMP_BUFFER_SIZE];

// Format timestamp as ISO 8601
inline void format_timestamp(char* buffer, size_t buffer_size) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    struct tm timeinfo;
#ifdef _WIN32
    gmtime_s(&timeinfo, &time_t);
#else
    gmtime_r(&time_t, &timeinfo);
#endif

    auto result = std::format_to_n(
        buffer, buffer_size - 1, "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
        timeinfo.tm_min, timeinfo.tm_sec, static_cast<int>(ms.count()));
    *result.out = '\0';
}

// Get log level string
inline constexpr const char* level_string(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR:
            return "ERROR";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::TRACE:
            return "TRACE";
        default:
            return "UNKNOWN";
    }
}

// Get log level string (lowercase for JSON)
inline constexpr const char* level_string_lower(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR:
            return "error";
        case LogLevel::WARN:
            return "warn";
        case LogLevel::INFO:
            return "info";
        case LogLevel::DEBUG:
            return "debug";
        case LogLevel::TRACE:
            return "trace";
        default:
            return "unknown";
    }
}

// Get syslog priority from log level
inline constexpr int syslog_priority(LogLevel level) {
#ifdef _WIN32
    // Windows doesn't have syslog, use numeric values
    switch (level) {
        case LogLevel::ERROR:
            return 3; // LOG_ERR equivalent
        case LogLevel::WARN:
            return 4; // LOG_WARNING equivalent
        case LogLevel::INFO:
            return 6; // LOG_INFO equivalent
        case LogLevel::DEBUG:
            return 7; // LOG_DEBUG equivalent
        case LogLevel::TRACE:
            return 7; // LOG_DEBUG equivalent
        default:
            return 6; // LOG_INFO equivalent
    }
#else
    // Use actual syslog constants
    switch (level) {
        case LogLevel::ERROR:
            return LOG_ERR;
        case LogLevel::WARN:
            return LOG_WARNING;
        case LogLevel::INFO:
            return LOG_INFO;
        case LogLevel::DEBUG:
            return LOG_DEBUG;
        case LogLevel::TRACE:
            return LOG_DEBUG;
        default:
            return LOG_INFO;
    }
#endif
}

// JSON escaping
inline void json_escape(std::string_view input, char* output, size_t& offset, size_t max_size) {
    for (char c : input) {
        if (offset >= max_size - 1)
            break;
        switch (c) {
            case '"':
                if (offset + 2 < max_size) {
                    output[offset++] = '\\';
                    output[offset++] = '"';
                }
                break;
            case '\\':
                if (offset + 2 < max_size) {
                    output[offset++] = '\\';
                    output[offset++] = '\\';
                }
                break;
            case '\n':
                if (offset + 2 < max_size) {
                    output[offset++] = '\\';
                    output[offset++] = 'n';
                }
                break;
            case '\r':
                if (offset + 2 < max_size) {
                    output[offset++] = '\\';
                    output[offset++] = 'r';
                }
                break;
            case '\t':
                if (offset + 2 < max_size) {
                    output[offset++] = '\\';
                    output[offset++] = 't';
                }
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control character - escape as \uXXXX
                    if (offset + 6 < max_size) {
                        auto result = std::format_to_n(output + offset, max_size - offset,
                                                       "\\u{:04x}", static_cast<unsigned char>(c));
                        offset += result.size;
                    }
                } else {
                    output[offset++] = c;
                }
                break;
        }
    }
}

// Format message to JSON
template <typename Formatter>
inline void format_json(LogLevel level, const char* timestamp, const Formatter& formatter,
                        char* buffer, size_t buffer_size, size_t& offset) {
    offset = 0;
    auto result1 = std::format_to_n(buffer + offset, buffer_size - offset,
                                    R"({{"timestamp":"{}","level":"{}","message":")", timestamp,
                                    level_string_lower(level));
    offset += result1.size;

    // Format message to temporary buffer, then escape and append
    char temp_buffer[detail::MAX_LOG_MESSAGE_SIZE];
    size_t temp_offset = 0;
    formatter(temp_buffer, sizeof(temp_buffer), temp_offset);
    temp_buffer[temp_offset] = '\0';

    // Escape message into a separate buffer, then append
    char escaped_buffer[detail::MAX_LOG_MESSAGE_SIZE * 2];
    size_t escaped_offset = 0;
    json_escape(std::string_view(temp_buffer, temp_offset), escaped_buffer, escaped_offset,
                sizeof(escaped_buffer));
    escaped_buffer[escaped_offset] = '\0';

    // Append escaped message
    size_t copy_size = std::min(escaped_offset, buffer_size - offset - 1);
    std::memcpy(buffer + offset, escaped_buffer, copy_size);
    offset += copy_size;

    auto result2 = std::format_to_n(buffer + offset, buffer_size - offset, "\"}}");
    offset += result2.size;
}

// Format message to syslog format (RFC 3164)
template <typename Formatter>
inline void format_syslog_rfc3164(LogLevel level, const char* timestamp, const Formatter& formatter,
                                  char* buffer, size_t buffer_size, size_t& offset) {
    offset = 0;
    // Format: <PRI>TIMESTAMP HOSTNAME TAG: MESSAGE
    // PRI = facility * 8 + severity
    // We use facility 16 (local0) for user-level messages
    int priority = 16 * 8 + syslog_priority(level);

    auto result1 =
        std::format_to_n(buffer + offset, buffer_size - offset, "<{}>{} ", priority, timestamp);
    offset += result1.size;
    auto result2 =
        std::format_to_n(buffer + offset, buffer_size - offset, "localhost libspaznet: ");
    offset += result2.size;
    // Message will be appended by formatter
}

// Format message to text
template <typename Formatter>
inline void format_text(LogLevel level, const char* timestamp, const Formatter& formatter,
                        char* buffer, size_t buffer_size, size_t& offset) {
    offset = 0;
    auto result = std::format_to_n(buffer + offset, buffer_size - offset, "[{}] [{}] ", timestamp,
                                   level_string(level));
    offset += result.size;
    formatter(buffer, buffer_size, offset);
}

} // namespace detail

// Logger class - singleton pattern
class Logger {
  public:
    // Get singleton instance
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // Initialize logger
    void initialize(LogLevel level = LogLevel::INFO, LogBackend backend = LogBackend::STDOUT,
                    LogFormat format = LogFormat::TEXT, const char* ident = "libspaznet") {
        level_ = level;
        backend_ = backend;
        format_ = format;
        ident_ = ident;

        if (backend_ == LogBackend::SYSLOG) {
#ifdef _WIN32
            // Windows doesn't have syslog, fall back to stdout
            backend_ = LogBackend::STDOUT;
#else
            // Open syslog connection
            openlog(ident_, LOG_PID | LOG_CONS, LOG_USER);
#endif
        }
    }

    // Set log level at runtime
    void set_level(LogLevel level) {
        level_ = level;
    }

    // Get current log level
    LogLevel get_level() const {
        return level_;
    }

    // Check if level should be logged (runtime gate)
    bool should_log(LogLevel level) const {
        return static_cast<uint8_t>(level) <= static_cast<uint8_t>(level_);
    }

    // Log message with deferred formatting
    template <typename Formatter> void log(LogLevel level, const Formatter& formatter) {
        // Runtime gate check
        if (!should_log(level)) {
            return;
        }

        // Format timestamp
        detail::format_timestamp(detail::timestamp_buffer, detail::TIMESTAMP_BUFFER_SIZE);

        // Format message using stack buffer
        char message_buffer[detail::MAX_LOG_MESSAGE_SIZE];
        size_t message_offset = 0;

        // Apply formatter based on format type
        switch (format_) {
            case LogFormat::JSON:
                detail::format_json(level, detail::timestamp_buffer, formatter, message_buffer,
                                    detail::MAX_LOG_MESSAGE_SIZE, message_offset);
                break;
            case LogFormat::SYSLOG:
                detail::format_syslog_rfc3164(level, detail::timestamp_buffer, formatter,
                                              message_buffer, detail::MAX_LOG_MESSAGE_SIZE,
                                              message_offset);
                formatter(message_buffer, detail::MAX_LOG_MESSAGE_SIZE, message_offset);
                break;
            case LogFormat::TEXT:
            default:
                detail::format_text(level, detail::timestamp_buffer, formatter, message_buffer,
                                    detail::MAX_LOG_MESSAGE_SIZE, message_offset);
                break;
        }

        // Output message
        if (message_offset > 0 && message_offset < detail::MAX_LOG_MESSAGE_SIZE) {
            message_buffer[message_offset] = '\n';
            message_offset++;
            message_buffer[message_offset] = '\0';

            if (backend_ == LogBackend::SYSLOG) {
#ifndef _WIN32
                // Use syslog with appropriate priority
                syslog(detail::syslog_priority(level), "%s", message_buffer);
#endif
            } else {
                // Output to stdout/stderr based on level
                FILE* stream = (level == LogLevel::ERROR) ? stderr : stdout;
                std::fwrite(message_buffer, 1, message_offset, stream);
                std::fflush(stream);
            }
        }
    }

  private:
    Logger() = default;
    ~Logger() {
        if (backend_ == LogBackend::SYSLOG) {
#ifndef _WIN32
            closelog();
#endif
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel level_ = LogLevel::INFO;
    LogBackend backend_ = LogBackend::STDOUT;
    LogFormat format_ = LogFormat::TEXT;
    const char* ident_ = "libspaznet";
};

// Helper for format string formatting with variadic arguments
namespace detail {
template <typename... Args>
inline void format_message_impl(char* buf, size_t buf_size, size_t& off, std::string_view fmt,
                                Args&&... args) {
    auto result = std::format_to_n(buf + off, buf_size - off, fmt, std::forward<Args>(args)...);
    off += result.size;
}
} // namespace detail

// Helper function to format simple string view
struct SimpleFormatter {
    std::string_view message;

    void operator()(char* buffer, size_t buffer_size, size_t& offset) const {
        size_t copy_size = std::min(message.size(), buffer_size - offset - 1);
        std::memcpy(buffer + offset, message.data(), copy_size);
        offset += copy_size;
    }
};

// Convenience macros for logging - using std::format syntax (C++20)
// Note: Users should use {} placeholders instead of %s, %d, etc.
#define SPAZNET_LOG(level, fmt, ...)                                                               \
    do {                                                                                           \
        auto& logger = spaznet::Logger::instance();                                                \
        if (logger.should_log(level)) {                                                            \
            logger.log(level, [&](char* buf, size_t buf_size, size_t& off) {                       \
                auto result =                                                                      \
                    std::format_to_n(buf + off, buf_size - off, fmt __VA_OPT__(, ) __VA_ARGS__);   \
                off += result.size;                                                                \
            });                                                                                    \
        }                                                                                          \
    } while (0)

// Runtime-gated logs
#define SPAZNET_ERROR(...) SPAZNET_LOG(spaznet::LogLevel::ERROR, __VA_ARGS__)
#define SPAZNET_WARN(...) SPAZNET_LOG(spaznet::LogLevel::WARN, __VA_ARGS__)
#define SPAZNET_INFO(...) SPAZNET_LOG(spaznet::LogLevel::INFO, __VA_ARGS__)

// Compile-time and runtime gated logs
#if ENABLE_DEBUG_LOGS
#define SPAZNET_DEBUG(...) SPAZNET_LOG(spaznet::LogLevel::DEBUG, __VA_ARGS__)
#else
#define SPAZNET_DEBUG(...)                                                                         \
    do {                                                                                           \
    } while (0)
#endif

#if ENABLE_TRACE_LOGS
#define SPAZNET_TRACE(...) SPAZNET_LOG(spaznet::LogLevel::TRACE, __VA_ARGS__)
#else
#define SPAZNET_TRACE(...)                                                                         \
    do {                                                                                           \
    } while (0)
#endif

// NOLINTEND

} // namespace spaznet
