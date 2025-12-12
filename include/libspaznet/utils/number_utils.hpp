#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace spaznet {

class NumberUtils {
  public:
    // Parse unsigned 64-bit integer; returns std::nullopt on failure
    static auto parse_uint64(const std::string& value) -> std::optional<uint64_t>;

    // Parse signed 32-bit integer; returns std::nullopt on failure
    static auto parse_int(const std::string& value) -> std::optional<int>;
};

} // namespace spaznet
