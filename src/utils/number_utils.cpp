#include <libspaznet/utils/number_utils.hpp>

#include <cstdint>
#include <stdexcept>

namespace spaznet {

std::optional<uint64_t> NumberUtils::parse_uint64(const std::string& value) {
    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> NumberUtils::parse_int(const std::string& value) {
    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace spaznet

