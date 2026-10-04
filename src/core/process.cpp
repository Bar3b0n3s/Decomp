#include "core/process.hpp"
#include "core/types.hpp"

#include <cstdlib>

#ifdef _WIN32
#include "core/strings.hpp"
#include <windows.h>
#endif

namespace decomp {

std::string quote_windows_arg(std::string_view arg) {
    bool needs_quotes = arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string_view::npos;
    if (!needs_quotes) return std::string(arg);
    std::string out = "\"";
    usize backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string build_windows_command_line(std::span<const std::string> argv) {
    std::string out;
    for (usize i = 0; i < argv.size(); ++i) {
        if (i) out += ' ';
        out += quote_windows_arg(argv[i]);
    }
    return out;
}

char path_list_separator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::optional<std::string> get_env(std::string_view name) {
#ifdef _WIN32
    std::wstring wname = utf8_to_wide(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    value.resize(n);
    return wide_to_utf8(value);
#else
    const char* v = std::getenv(std::string(name).c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

} // namespace decomp
