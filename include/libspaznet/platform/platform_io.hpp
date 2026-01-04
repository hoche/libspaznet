#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace spaznet {

// Platform-agnostic I/O interface
class PlatformIO {
  public:
    struct Event {
        int fd;
        uint32_t events; // Read, Write, Error flags
        void* user_data;
    };

    virtual ~PlatformIO() = default;

    // Default constructor
    PlatformIO() = default;

    // Delete copy and move operations
    PlatformIO(const PlatformIO&) = delete;
    auto operator=(const PlatformIO&) -> PlatformIO& = delete;
    PlatformIO(PlatformIO&&) = delete;
    auto operator=(PlatformIO&&) -> PlatformIO& = delete;

    // Initialize the I/O system
    virtual auto init() -> bool = 0;

    // Register a file descriptor for events
    virtual auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool = 0;

    // Modify events for a file descriptor
    virtual auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool = 0;

    // Remove a file descriptor
    virtual auto remove_fd(int file_descriptor) -> bool = 0;

    // Wait for events (blocking)
    // Returns number of events, or -1 on error
    virtual auto wait(std::vector<Event>& events, int timeout_ms) -> int = 0;

    // Cleanup
    virtual auto cleanup() -> void = 0;

    // Event flags
    static constexpr uint32_t EVENT_READ = 0x01;
    static constexpr uint32_t EVENT_WRITE = 0x02;
    static constexpr uint32_t EVENT_ERROR = 0x04;
    static constexpr uint32_t EVENT_EDGE_TRIGGER = 0x08;
};

// Factory function
auto create_platform_io() -> std::unique_ptr<PlatformIO>;

} // namespace spaznet
