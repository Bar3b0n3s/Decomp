#include "agent/http.hpp"

#include "core/strings.hpp"

namespace decomp::agent {

std::optional<std::string> find_header(const std::vector<HttpHeader>& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (iequals(key, name)) return value;
    }
    return std::nullopt;
}

} // namespace decomp::agent
