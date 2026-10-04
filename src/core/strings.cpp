#include "core/strings.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace decomp {

std::string_view trim(std::string_view s) {
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    usize start = 0;
    while (true) {
        usize pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    for (auto line : split(s, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.emplace_back(line);
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    std::string out;
    usize pos = 0;
    while (true) {
        usize hit = s.find(from, pos);
        if (hit == std::string_view::npos) break;
        out.append(s.substr(pos, hit - pos));
        out.append(to);
        pos = hit + from.size();
    }
    out.append(s.substr(pos));
    return out;
}

std::optional<u64> parse_u64(std::string_view s) {
    s = trim(s);
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    } else if (s.size() > 1 && (s.back() == 'h' || s.back() == 'H')) {
        s.remove_suffix(1);
        base = 16;
    }
    if (s.empty()) return std::nullopt;
    u64 value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

std::string hex(u64 value, int width) {
    if (width <= 0) return std::format("{:#x}", value);
    return std::format("0x{:0{}x}", value, width);
}

std::string hex_bytes(const u8* data, usize size, std::string_view sep) {
    std::string out;
    for (usize i = 0; i < size; ++i) {
        if (i) out += sep;
        out += std::format("{:02X}", data[i]);
    }
    return out;
}

std::string truncate_utf8(std::string_view s, usize max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    usize cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return std::format("{}\n... [truncated {} of {} bytes]", s.substr(0, cut), s.size() - cut, s.size());
}

std::string escape_c_string(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default:
            if (c < 0x20 || c == 0x7F) out += std::format("\\x{:02x}", c);
            else out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<usize>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
#endif

} // namespace decomp
