#include <libspaznet/utils/string_utils.hpp>

#include <algorithm>
#include <cctype>

namespace spaznet {

auto StringUtils::to_lower(const std::string& input) -> std::string {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

auto StringUtils::trim_ows(const std::string& input) -> std::string {
    size_t start = 0;
    while (start < input.size() && (input[start] == ' ' || input[start] == '\t')) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t')) {
        --end;
    }
    return input.substr(start, end - start);
}

} // namespace spaznet
