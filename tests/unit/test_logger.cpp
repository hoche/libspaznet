#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <format>
#include <libspaznet/logger.hpp>
#include <string>
#include <thread>

using namespace spaznet;

class LoggerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Reset logger to defaults before each test
        logger_ = &Logger::instance();
        logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::TEXT, "test");
    }

    Logger* logger_;
};

// Test log level enum values
TEST(LoggerTest, LogLevelValues) {
    EXPECT_EQ(static_cast<uint8_t>(LogLevel::NONE), 0);
    EXPECT_EQ(static_cast<uint8_t>(LogLevel::ERROR), 1);
    EXPECT_EQ(static_cast<uint8_t>(LogLevel::WARN), 2);
    EXPECT_EQ(static_cast<uint8_t>(LogLevel::INFO), 3);
    EXPECT_EQ(static_cast<uint8_t>(LogLevel::DEBUG), 4);
    EXPECT_EQ(static_cast<uint8_t>(LogLevel::TRACE), 5);
}

// Test singleton pattern
TEST(LoggerTest, SingletonPattern) {
    Logger& logger1 = Logger::instance();
    Logger& logger2 = Logger::instance();
    EXPECT_EQ(&logger1, &logger2);
}

// Test initialization
TEST_F(LoggerTest, Initialization) {
    logger_->initialize(LogLevel::WARN, LogBackend::STDOUT, LogFormat::JSON, "custom_ident");
    EXPECT_EQ(logger_->get_level(), LogLevel::WARN);

    logger_->initialize(LogLevel::DEBUG, LogBackend::SYSLOG, LogFormat::TEXT, "another_ident");
    EXPECT_EQ(logger_->get_level(), LogLevel::DEBUG);
}

// Test set/get log level
TEST_F(LoggerTest, SetGetLevel) {
    logger_->set_level(LogLevel::ERROR);
    EXPECT_EQ(logger_->get_level(), LogLevel::ERROR);

    logger_->set_level(LogLevel::TRACE);
    EXPECT_EQ(logger_->get_level(), LogLevel::TRACE);
}

// Test runtime gate - should_log
TEST_F(LoggerTest, RuntimeGate) {
    logger_->set_level(LogLevel::INFO);

    EXPECT_TRUE(logger_->should_log(LogLevel::ERROR));
    EXPECT_TRUE(logger_->should_log(LogLevel::WARN));
    EXPECT_TRUE(logger_->should_log(LogLevel::INFO));
    EXPECT_FALSE(logger_->should_log(LogLevel::DEBUG));
    EXPECT_FALSE(logger_->should_log(LogLevel::TRACE));

    logger_->set_level(LogLevel::DEBUG);
    EXPECT_TRUE(logger_->should_log(LogLevel::DEBUG));
    EXPECT_FALSE(logger_->should_log(LogLevel::TRACE));

    logger_->set_level(LogLevel::NONE);
    EXPECT_FALSE(logger_->should_log(LogLevel::ERROR));
}

// Test TEXT format - verify it doesn't crash
TEST_F(LoggerTest, TextFormat) {
    logger_->set_level(LogLevel::INFO);
    logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::TEXT, "test");

    // Verify it doesn't crash
    EXPECT_NO_THROW({
        logger_->log(LogLevel::INFO, [](char* buf, size_t buf_size, size_t& off) {
            auto result = std::format_to_n(buf + off, buf_size - off, "Test message");
            off += result.size;
        });
    });
}

// Test JSON format - verify it doesn't crash
TEST_F(LoggerTest, JsonFormat) {
    logger_->set_level(LogLevel::INFO);
    logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::JSON, "test");

    EXPECT_NO_THROW({
        logger_->log(LogLevel::INFO, [](char* buf, size_t buf_size, size_t& off) {
            auto result = std::format_to_n(buf + off, buf_size - off, "JSON test");
            off += result.size;
        });
    });
}

// Test SYSLOG format - verify it doesn't crash
TEST_F(LoggerTest, SyslogFormat) {
    logger_->set_level(LogLevel::INFO);
    logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::SYSLOG, "test");

    EXPECT_NO_THROW({
        logger_->log(LogLevel::INFO, [](char* buf, size_t buf_size, size_t& off) {
            auto result = std::format_to_n(buf + off, buf_size - off, "Syslog test");
            off += result.size;
        });
    });
}

// Test log level filtering - INFO level should not log DEBUG
TEST_F(LoggerTest, LogLevelFiltering) {
    logger_->set_level(LogLevel::INFO);

    bool formatter_called = false;

    // This should not call the formatter
    logger_->log(LogLevel::DEBUG, [&](char* buf, size_t buf_size, size_t& off) {
        formatter_called = true;
        auto result = std::format_to_n(buf + off, buf_size - off, "This should not appear");
        off += result.size;
    });

    EXPECT_FALSE(formatter_called);
}

// Test that ERROR logs don't crash
TEST_F(LoggerTest, ErrorLogging) {
    logger_->set_level(LogLevel::ERROR);
    logger_->initialize(LogLevel::ERROR, LogBackend::STDOUT, LogFormat::TEXT, "test");

    EXPECT_NO_THROW({
        logger_->log(LogLevel::ERROR, [](char* buf, size_t buf_size, size_t& off) {
            auto result = std::format_to_n(buf + off, buf_size - off, "Error message");
            off += result.size;
        });
    });
}

// Test that non-ERROR logs don't crash
TEST_F(LoggerTest, NonErrorLogging) {
    logger_->set_level(LogLevel::INFO);
    logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::TEXT, "test");

    EXPECT_NO_THROW({
        logger_->log(LogLevel::INFO, [](char* buf, size_t buf_size, size_t& off) {
            auto result = std::format_to_n(buf + off, buf_size - off, "Info message");
            off += result.size;
        });
    });
}

// Test deferred formatting - verify formatter is only called if level matches
TEST_F(LoggerTest, DeferredFormatting) {
    logger_->set_level(LogLevel::INFO);

    bool formatter_called = false;

    // This should not call the formatter
    logger_->log(LogLevel::DEBUG, [&](char* buf, size_t buf_size, size_t& off) {
        formatter_called = true;
        auto result = std::format_to_n(buf + off, buf_size - off, "Debug message");
        off += result.size;
    });

    EXPECT_FALSE(formatter_called);

    // This should call the formatter
    logger_->log(LogLevel::INFO, [&](char* buf, size_t buf_size, size_t& off) {
        formatter_called = true;
        auto result = std::format_to_n(buf + off, buf_size - off, "Info message");
        off += result.size;
    });

    EXPECT_TRUE(formatter_called);
}

// Test JSON escaping
TEST(LoggerTest, JsonEscaping) {
    using namespace spaznet::detail;

    char output[256];
    size_t offset = 0;

    // Test newline escaping
    json_escape(std::string_view("Hello\nWorld"), output, offset, sizeof(output));
    output[offset] = '\0';
    EXPECT_GT(offset, 11); // Should be longer due to escaping
    EXPECT_NE(std::strstr(output, "\\n"), nullptr);

    // Reset and test quote escaping
    offset = 0;
    json_escape(std::string_view("Quote\"Test"), output, offset, sizeof(output));
    output[offset] = '\0';
    EXPECT_GT(offset, 10);
    EXPECT_NE(std::strstr(output, "\\\""), nullptr);

    // Reset and test backslash escaping
    offset = 0;
    json_escape(std::string_view("Back\\slash"), output, offset, sizeof(output));
    output[offset] = '\0';
    EXPECT_GT(offset, 10);
    EXPECT_NE(std::strstr(output, "\\\\"), nullptr);

    // Reset and test tab escaping
    offset = 0;
    json_escape(std::string_view("Tab\tTest"), output, offset, sizeof(output));
    output[offset] = '\0';
    EXPECT_GT(offset, 8);
    EXPECT_NE(std::strstr(output, "\\t"), nullptr);

    // Reset and test carriage return escaping
    offset = 0;
    json_escape(std::string_view("CR\rTest"), output, offset, sizeof(output));
    output[offset] = '\0';
    EXPECT_GT(offset, 7);
    EXPECT_NE(std::strstr(output, "\\r"), nullptr);
}

// Test log level strings
TEST(LoggerTest, LogLevelStrings) {
    using namespace spaznet::detail;

    EXPECT_STREQ(level_string(LogLevel::ERROR), "ERROR");
    EXPECT_STREQ(level_string(LogLevel::WARN), "WARN");
    EXPECT_STREQ(level_string(LogLevel::INFO), "INFO");
    EXPECT_STREQ(level_string(LogLevel::DEBUG), "DEBUG");
    EXPECT_STREQ(level_string(LogLevel::TRACE), "TRACE");
}

// Test log level strings (lowercase)
TEST(LoggerTest, LogLevelStringsLower) {
    using namespace spaznet::detail;

    EXPECT_STREQ(level_string_lower(LogLevel::ERROR), "error");
    EXPECT_STREQ(level_string_lower(LogLevel::WARN), "warn");
    EXPECT_STREQ(level_string_lower(LogLevel::INFO), "info");
    EXPECT_STREQ(level_string_lower(LogLevel::DEBUG), "debug");
    EXPECT_STREQ(level_string_lower(LogLevel::TRACE), "trace");
}

// Test syslog priority mapping
TEST(LoggerTest, SyslogPriority) {
    using namespace spaznet::detail;

#ifndef _WIN32
    EXPECT_EQ(syslog_priority(LogLevel::ERROR), LOG_ERR);
    EXPECT_EQ(syslog_priority(LogLevel::WARN), LOG_WARNING);
    EXPECT_EQ(syslog_priority(LogLevel::INFO), LOG_INFO);
    EXPECT_EQ(syslog_priority(LogLevel::DEBUG), LOG_DEBUG);
    EXPECT_EQ(syslog_priority(LogLevel::TRACE), LOG_DEBUG);
#else
    // Windows doesn't have syslog, test numeric values
    EXPECT_EQ(syslog_priority(LogLevel::ERROR), 3);
    EXPECT_EQ(syslog_priority(LogLevel::WARN), 4);
    EXPECT_EQ(syslog_priority(LogLevel::INFO), 6);
    EXPECT_EQ(syslog_priority(LogLevel::DEBUG), 7);
    EXPECT_EQ(syslog_priority(LogLevel::TRACE), 7);
#endif
}

// Test timestamp formatting
TEST(LoggerTest, TimestampFormatting) {
    using namespace spaznet::detail;

    format_timestamp(detail::timestamp_buffer, detail::TIMESTAMP_BUFFER_SIZE);

    // Verify timestamp format: YYYY-MM-DDTHH:MM:SS.mmmZ
    std::string ts(detail::timestamp_buffer);
    EXPECT_EQ(ts.length(), 24); // ISO 8601 format with milliseconds
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[7], '-');
    EXPECT_EQ(ts[10], 'T');
    EXPECT_EQ(ts[13], ':');
    EXPECT_EQ(ts[16], ':');
    EXPECT_EQ(ts[19], '.');
    EXPECT_EQ(ts[23], 'Z');
}

// Test macros compile and work
TEST_F(LoggerTest, MacroUsage) {
    logger_->set_level(LogLevel::INFO);

    bool debug_called = false;
    bool trace_called = false;

    EXPECT_NO_THROW({
        SPAZNET_ERROR("Error: {}", "test");
        SPAZNET_WARN("Warning: {}", 42);
        SPAZNET_INFO("Info: {}", "test");

        // These should not execute at INFO level (compile-time gated or runtime gated)
        SPAZNET_DEBUG("Debug: {}", 123);
        SPAZNET_TRACE("Trace: {}", "test");
    });
}

// Test multiple format specifiers
TEST_F(LoggerTest, MultipleFormatSpecifiers) {
    logger_->set_level(LogLevel::INFO);

    EXPECT_NO_THROW({ SPAZNET_INFO("Formatted: {} {} {:.2f}", "test", 42, 3.14); });
}

// Test empty message (use format specifier to avoid zero-length format string warning)
TEST_F(LoggerTest, EmptyMessage) {
    logger_->set_level(LogLevel::INFO);

    EXPECT_NO_THROW({ SPAZNET_INFO("{}", ""); });
}

// Test long message (should not overflow buffer)
TEST_F(LoggerTest, LongMessage) {
    logger_->set_level(LogLevel::INFO);

    EXPECT_NO_THROW({
        std::string long_msg(5000, 'A');
        SPAZNET_INFO("Long: {}", long_msg);
    });
}

// Test changing format at runtime
TEST_F(LoggerTest, ChangeFormatAtRuntime) {
    logger_->set_level(LogLevel::INFO);

    EXPECT_NO_THROW({
        logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::TEXT, "test");
        SPAZNET_INFO("Text format");

        logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::JSON, "test");
        SPAZNET_INFO("JSON format");

        logger_->initialize(LogLevel::INFO, LogBackend::STDOUT, LogFormat::SYSLOG, "test");
        SPAZNET_INFO("Syslog format");
    });
}

// Test Windows syslog fallback (if compiled on Windows)
#ifdef _WIN32
TEST_F(LoggerTest, WindowsSyslogFallback) {
    logger_->initialize(LogLevel::INFO, LogBackend::SYSLOG, LogFormat::TEXT, "test");
    // Should fall back to STDOUT on Windows
    // Just verify it doesn't crash
    SPAZNET_INFO("Should use stdout");
}
#endif

// Test thread-local timestamp buffer - verify timestamps are valid
TEST_F(LoggerTest, ThreadLocalTimestamp) {
    logger_->set_level(LogLevel::INFO);

    using namespace spaznet::detail;

    // Get initial timestamp
    format_timestamp(detail::timestamp_buffer, detail::TIMESTAMP_BUFFER_SIZE);
    std::string ts1(detail::timestamp_buffer);
    EXPECT_EQ(ts1.length(), 24);

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Get another timestamp
    format_timestamp(detail::timestamp_buffer, detail::TIMESTAMP_BUFFER_SIZE);
    std::string ts2(detail::timestamp_buffer);
    EXPECT_EQ(ts2.length(), 24);

    // Timestamps should be valid even if same (fast execution) or different
    EXPECT_GE(ts2, ts1);

    EXPECT_NO_THROW({
        SPAZNET_INFO("Message 1");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        SPAZNET_INFO("Message 2");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        SPAZNET_INFO("Message 3");
    });
}
