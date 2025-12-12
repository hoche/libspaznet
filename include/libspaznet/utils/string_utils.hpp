#pragma once

#include <string>

namespace spaznet {

class StringUtils {
  public:
    // Convert string to lowercase (ASCII)
    static std::string to_lower(const std::string& input);

    // Trim optional whitespace (OWS = SP / HTAB) from both ends
    static std::string trim_ows(const std::string& input);
};

} // namespace spaznet
