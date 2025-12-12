#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace spaznet {

class HeaderUtils {
  public:
    // Case-insensitive header lookup for HTTP/1.x
    static auto get_header_case_insensitive(
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& name) -> std::optional<std::string>;
};

} // namespace spaznet
