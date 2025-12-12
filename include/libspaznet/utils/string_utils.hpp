#pragma once

#include <string>

namespace spaznet {

class StringUtils {
  public:
    // Convert string to lowercase (ASCII)
    static auto to_lower(const std::string& input) -> std::string;

    // Trim optional whitespace (OWS = SP / HTAB) from both ends
    static auto trim_ows(const std::string& input) -> std::string;
};

} // namespace spaznet
