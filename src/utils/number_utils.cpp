#include <libspaznet/utils/number_utils.hpp>

#include <cstdint>
#include <stdexcept>

namespace spaznet {

auto NumberUtils::parse_uint64(const std::string& value) -> std::optional<uint64_t> {
    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (...) {
        return std::nullopt;
    }
}

auto NumberUtils::parse_int(const std::string& value) -> std::optional<int> {
    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace spaznet
