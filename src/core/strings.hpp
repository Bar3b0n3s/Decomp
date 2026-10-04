#pragma once

#include "core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

std::string_view trim(std::string_view s);
std::vector<std::string_view> split(std::string_view s, char sep);
std::vector<std::string> split_lines(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);

// "0x1234", "1234h" (hex) or decimal.
std::optional<u64> parse_u64(std::string_view s);
// Hex, zero-padded to `width` digits, with 0x prefix: hex(0x401000, 8) == "0x00401000".
std::string hex(u64 value, int width = 0);
std::string hex_bytes(const u8* data, usize size, std::string_view sep = " ");

// Truncates to at most `max_bytes` bytes on a UTF-8 boundary, appending a note when cut.
std::string truncate_utf8(std::string_view s, usize max_bytes);

// Escapes control characters and quotes for display: hello\n -> "hello\\n".
std::string escape_c_string(std::string_view s);

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view s);
std::string wide_to_utf8(std::wstring_view s);
#endif

} // namespace decomp
