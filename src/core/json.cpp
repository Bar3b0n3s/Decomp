#include "core/json.hpp"

namespace decomp {

Result<Json> parse_json(std::string_view text) {
    try {
        return Json::parse(text);
    } catch (const Json::parse_error& e) {
        return make_error(ErrorCode::parse, "invalid JSON: {}", e.what());
    }
}

std::string dump_compact(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string dump_pretty(const Json& value) {
    return value.dump(2, ' ', false, Json::error_handler_t::replace);
}

Result<std::string> json_string(const Json& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return make_error(ErrorCode::parse, "missing string field '{}'", key);
    return it->get<std::string>();
}

Result<long long> json_int(const Json& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return make_error(ErrorCode::parse, "missing integer field '{}'", key);
    return it->get<long long>();
}

std::string json_string_or(const Json& obj, std::string_view key, std::string_view fallback) {
    if (!obj.is_object()) return std::string(fallback);
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
}

long long json_int_or(const Json& obj, std::string_view key, long long fallback) {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    return it != obj.end() && it->is_number_integer() ? it->get<long long>() : fallback;
}

double json_number_or(const Json& obj, std::string_view key, double fallback) {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<double>() : fallback;
}

bool json_bool_or(const Json& obj, std::string_view key, bool fallback) {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

} // namespace decomp
