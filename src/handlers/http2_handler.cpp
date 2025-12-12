#include <cstring>
#include <libspaznet/handlers/http2_handler.hpp>
#include <sstream>

namespace spaznet {

HTTP2Frame HTTP2Response::to_frame() const {
    HTTP2Frame frame;
    frame.stream_id = stream_id;
    frame.type = 0x01;  // HEADERS frame
    frame.flags = 0x04; // END_HEADERS

    // Simplified - in a real implementation, you'd use HPACK encoding
    std::ostringstream oss;
    oss << ":status: " << status_code << "\r\n";
    for (const auto& [key, value] : headers) {
        oss << key << ": " << value << "\r\n";
    }

    std::string header_str = oss.str();
    frame.payload.assign(header_str.begin(), header_str.end());
    frame.payload.insert(frame.payload.end(), body.begin(), body.end());
    frame.length = frame.payload.size();

    return frame;
}

} // namespace spaznet

