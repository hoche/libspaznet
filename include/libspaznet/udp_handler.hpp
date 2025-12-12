#pragma once

#include <libspaznet/io_context.hpp>
#include <vector>
#include <cstdint>
#include <string>

namespace spaznet {

struct UDPPacket {
    std::vector<uint8_t> data;
    std::string address;
    uint16_t port;
};

class UDPHandler {
public:
    virtual ~UDPHandler() = default;
    
    // Handle incoming UDP packet
    virtual Task handle_packet(const UDPPacket& packet, Socket& socket) = 0;
};

} // namespace spaznet

