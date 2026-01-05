#include <cctype>
#include <libspaznet/utils/header_utils.hpp>

namespace spaznet {

namespace {

[[nodiscard]] inline auto ascii_iequals(const std::string& lhs, const std::string& rhs) -> bool {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < lhs.size(); ++idx) {
        const auto lhs_char = static_cast<unsigned char>(lhs[idx]);
        const auto rhs_char = static_cast<unsigned char>(rhs[idx]);
        if (std::tolower(lhs_char) != std::tolower(rhs_char)) {
            return false;
        }
    }
    return true;
}

} // namespace

auto HeaderUtils::get_header_case_insensitive(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& name) -> std::optional<std::string> {
    // Fast path: exact match.
    if (auto iter = headers.find(name); iter != headers.end()) {
        return iter->second;
    }

    // Slow path: allocation-free case-insensitive scan (ASCII).
    for (const auto& [key, value] : headers) {
        if (ascii_iequals(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace spaznet
