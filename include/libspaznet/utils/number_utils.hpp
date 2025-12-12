#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace spaznet {

class NumberUtils {
  public:
    // Parse unsigned 64-bit integer; returns std::nullopt on failure
    static std::optional<uint64_t> parse_uint64(const std::string& value);

    // Parse signed 32-bit integer; returns std::nullopt on failure
    static std::optional<int> parse_int(const std::string& value);
};

} // namespace spaznet

