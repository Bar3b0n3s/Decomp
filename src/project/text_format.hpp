#pragma once

// The line format symbols.txt and units.txt share: whitespace-separated tokens, `key=value` fields, and
// double quotes with C escapes around values that contain spaces, tabs, quotes or `=`.

#include <string>
#include <string_view>
#include <vector>

namespace decomp::project {

std::string quote_if_needed(const std::string& value);
// Splits a line into tokens; a double-quoted part may contain spaces and C escapes.
std::vector<std::string> tokenize(std::string_view line);

} // namespace decomp::project
