#include "formats/map.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"

#include <charconv>

namespace decomp::map {

namespace {

std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> out;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const usize start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

std::optional<u64> hex_value(std::string_view s) {
    if (s.ends_with('H') || s.ends_with('h')) s.remove_suffix(1);
    if (s.empty()) return std::nullopt;
    u64 v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

// "0001:000002de" -> (1, 0x2de)
std::optional<std::pair<u16, u32>> section_offset(std::string_view s) {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    auto sec = hex_value(s.substr(0, colon));
    auto off = hex_value(s.substr(colon + 1));
    if (!sec || !off || *sec > 0xFFFF || *off > 0xFFFFFFFF) return std::nullopt;
    return std::pair{static_cast<u16>(*sec), static_cast<u32>(*off)};
}

} // namespace

Result<MapFile> parse(std::string_view text) {
    MapFile m;
    enum class Part { header, sections, publics, statics, other } part = Part::header;
    bool first = true;
    usize line_no = 0;
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        ++line_no;
        if (line.ends_with('\r')) line.remove_suffix(1);
        const std::string_view trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (first) {
            m.module = std::string(trimmed);
            first = false;
            continue;
        }
        if (trimmed.starts_with("Preferred load address is")) {
            if (auto v = hex_value(trim(trimmed.substr(25)))) m.preferred_base = *v;
            continue;
        }
        if (trimmed.starts_with("Start") && trimmed.find("Length") != std::string_view::npos) {
            part = Part::sections;
            continue;
        }
        if (trimmed.find("Publics by Value") != std::string_view::npos) {
            part = Part::publics;
            continue;
        }
        if (trimmed == "Static symbols") {
            part = Part::statics;
            continue;
        }
        if (trimmed.starts_with("entry point at")) {
            auto t = tokens(trimmed);
            if (!t.empty()) m.entry_point = section_offset(t.back());
            part = Part::other;
            continue;
        }
        if (trimmed.starts_with("Exports") || trimmed.starts_with("FIXUPS:") || trimmed.starts_with("Line numbers")) {
            part = Part::other;
            continue;
        }
        const auto t = tokens(trimmed);
        if (part == Part::sections) {
            // 0001:00000000 000002deH .text                   CODE
            if (t.size() < 3) continue;
            auto so = section_offset(t[0]);
            auto len = hex_value(t[1]);
            if (!so || !len) continue;
            m.sections.push_back({so->first, so->second, static_cast<u32>(*len), std::string(t[2]), t.size() > 3 ? std::string(t[3]) : ""});
        } else if (part == Part::publics || part == Part::statics) {
            // 0001:00000000       _main                      00401000 f   main.obj
            if (t.size() < 3) continue;
            auto so = section_offset(t[0]);
            auto va = hex_value(t[2]);
            if (!so || !va) continue;
            Entry e;
            e.section = so->first;
            e.offset = so->second;
            e.name = std::string(t[1]);
            e.va = *va;
            e.is_static = part == Part::statics;
            for (usize k = 3; k < t.size(); ++k) {
                if (t[k] == "f") {
                    e.function = true;
                    m.has_function_flags = true;
                } else if (t[k] == "i") {
                    // inline/imported marker: no meaning here
                } else {
                    e.object = std::string(t[k]);
                }
            }
            m.entries.push_back(std::move(e));
        }
    }
    if (m.module.empty()) return make_error(ErrorCode::parse, "not a linker map file (empty)");
    if (m.entries.empty() && m.sections.empty()) return make_error(ErrorCode::parse, "not a linker map file: no sections or symbols found");
    return m;
}

Result<MapFile> load(const std::filesystem::path& path) {
    TRY_ASSIGN(auto text, fs::read_text(path));
    auto parsed = parse(text);
    if (!parsed) return make_error(parsed.error().code, "{}: {}", fs::to_utf8(path), parsed.error().message);
    return parsed;
}

} // namespace decomp::map
