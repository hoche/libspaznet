#include <algorithm>
#include <libspaznet/handlers/http_handler.hpp>
#include <sstream>

namespace spaznet {

std::vector<uint8_t> HTTPResponse::serialize() const {
    std::ostringstream oss;

    // Status line
    oss << "HTTP/" << (headers.count("HTTP-Version") ? headers.at("HTTP-Version") : "1.1") << " "
        << status_code << " " << status_message << "\r\n";

    // Headers
    for (const auto& [key, value] : headers) {
        if (key != "HTTP-Version") {
            oss << key << ": " << value << "\r\n";
        }
    }

    // Content-Length if not set
    if (!headers.count("Content-Length") && !body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }

    oss << "\r\n";

    std::string header_str = oss.str();
    std::vector<uint8_t> result;
    result.reserve(header_str.size() + body.size());

    result.insert(result.end(), header_str.begin(), header_str.end());
    result.insert(result.end(), body.begin(), body.end());

    return result;
}

} // namespace spaznet

