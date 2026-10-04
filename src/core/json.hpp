#pragma once

#include "core/result.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace decomp {

// nlohmann::json keeps object keys sorted (std::map), so dumps are deterministic.
using Json = nlohmann::json;

Result<Json> parse_json(std::string_view text);
std::string dump_compact(const Json& value);
std::string dump_pretty(const Json& value);

// Accessors that turn missing/mistyped fields into descriptive errors.
Result<std::string> json_string(const Json& obj, std::string_view key);
Result<long long> json_int(const Json& obj, std::string_view key);
std::string json_string_or(const Json& obj, std::string_view key, std::string_view fallback);
long long json_int_or(const Json& obj, std::string_view key, long long fallback);
double json_number_or(const Json& obj, std::string_view key, double fallback);
bool json_bool_or(const Json& obj, std::string_view key, bool fallback);

} // namespace decomp
