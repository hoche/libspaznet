#include <libspaznet/utils/header_utils.hpp>
#include <libspaznet/utils/string_utils.hpp>

namespace spaznet {

std::optional<std::string> HeaderUtils::get_header_case_insensitive(
    const std::unordered_map<std::string, std::string>& headers, const std::string& name) {
    const std::string lower_name = StringUtils::to_lower(name);
    for (const auto& [key, value] : headers) {
        if (StringUtils::to_lower(key) == lower_name) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace spaznet
