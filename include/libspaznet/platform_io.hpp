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
        uint32_t events;  // Read, Write, Error flags
        void* user_data;
    };
    
    virtual ~PlatformIO() = default;
    
    // Initialize the I/O system
    virtual bool init() = 0;
    
    // Register a file descriptor for events
    virtual bool add_fd(int fd, uint32_t events, void* user_data) = 0;
    
    // Modify events for a file descriptor
    virtual bool modify_fd(int fd, uint32_t events, void* user_data) = 0;
    
    // Remove a file descriptor
    virtual bool remove_fd(int fd) = 0;
    
    // Wait for events (blocking)
    // Returns number of events, or -1 on error
    virtual int wait(std::vector<Event>& events, int timeout_ms) = 0;
    
    // Cleanup
    virtual void cleanup() = 0;
    
    // Event flags
    static constexpr uint32_t EVENT_READ = 0x01;
    static constexpr uint32_t EVENT_WRITE = 0x02;
    static constexpr uint32_t EVENT_ERROR = 0x04;
    static constexpr uint32_t EVENT_EDGE_TRIGGER = 0x08;
};

// Factory function
std::unique_ptr<PlatformIO> create_platform_io();

} // namespace spaznet

