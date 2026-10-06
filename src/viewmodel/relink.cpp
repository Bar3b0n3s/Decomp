#include "viewmodel/relink.hpp"

#include "core/fs.hpp"

#include <algorithm>
#include <format>

namespace decomp::vm {

namespace {

std::string text_of(const Json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

template <class T>
T number_of(const Json& j, const char* key, T fallback = {}) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<T>() : fallback;
}

std::optional<u32> rva_of(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<u32>();
}

std::vector<std::string> strings_of(const Json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& s : *it)
        if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

const Json& array_of(const Json& j, const char* key) {
    static const Json empty = Json::array();
    auto it = j.find(key);
    return it != j.end() && it->is_array() ? *it : empty;
}

RelinkDifference read_difference(const Json& j) {
    RelinkDifference d;
    d.offset = number_of<u32>(j, "offset");
    d.size = number_of<u32>(j, "size");
    d.where = text_of(j, "where");
    d.rva = rva_of(j, "rva");
    d.unit = text_of(j, "unit");
    d.symbol = text_of(j, "symbol");
    d.original = text_of(j, "original");
    d.relinked = text_of(j, "relinked");
    return d;
}

std::optional<u8> hex_digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
    return std::nullopt;
}

std::optional<u8> byte_at(const pe::Image& image, i64 rva) {
    if (rva < 0 || rva > 0xFFFFFFFF) return std::nullopt;
    const auto offset = image.rva_to_offset(static_cast<u32>(rva));
    if (!offset) return std::nullopt;
    return static_cast<u8>(image.data()[*offset]);
}

} // namespace

usize RelinkUnitCheck::count(std::string_view state) const {
    return static_cast<usize>(std::ranges::count(sections, state, &RelinkPlacedSection::state));
}

RelinkUnitCheck read_unit_check(const Json& j) {
    RelinkUnitCheck c;
    c.complete = j.value("complete", false);
    c.summary = text_of(j, "summary");
    c.error = text_of(j, "error");
    c.compile_output = text_of(j, "compile_output");
    c.functions = number_of<usize>(j, "functions");
    for (const auto& va : array_of(j, "missing_functions"))
        if (va.is_number_unsigned()) c.missing_functions.push_back(va.get<u64>());
    auto check = j.find("check");
    if (check == j.end() || !check->is_object()) return c;
    for (const auto& s : array_of(*check, "sections")) {
        RelinkPlacedSection p;
        p.section = text_of(s, "section");
        p.name = text_of(s, "name");
        p.symbol = text_of(s, "symbol");
        p.state = text_of(s, "state");
        p.unit = text_of(s, "unit");
        p.folded_into = text_of(s, "folded_into");
        p.note = text_of(s, "note");
        p.size = number_of<u32>(s, "size");
        p.rva = rva_of(s, "rva");
        p.first_difference = rva_of(s, "first_difference");
        p.differing_bytes = number_of<u32>(s, "differing_bytes");
        p.comdat = s.value("comdat", false);
        c.sections.push_back(std::move(p));
    }
    for (const auto& m : array_of(*check, "missing"))
        c.missing.push_back({number_of<u32>(m, "rva"), number_of<u32>(m, "size"), text_of(m, "name"), text_of(m, "section")});
    c.problems = strings_of(*check, "problems");
    return c;
}

usize RelinkReport::count(std::string_view mode) const {
    return static_cast<usize>(std::ranges::count(units, mode, &RelinkUnit::mode));
}

std::string RelinkReport::headline() const {
    if (!link_ok) return error.empty() ? std::string("the link failed") : error;
    if (!compared) return error.empty() ? std::string("the relinked image was not compared") : error;
    if (identical) {
        const usize from_source = count("source");
        return std::format("identical to the target ({} unit{} from source, {} split)", from_source, from_source == 1 ? "" : "s", count("split"));
    }
    std::string out = std::format("differs from the target: {} byte{}", differing_bytes, differing_bytes == 1 ? "" : "s");
    if (first) {
        out += std::format(", the first at {}", first->where);
        if (!first->unit.empty()) out += " in " + first->unit;
        if (!first->symbol.empty()) out += " (" + first->symbol + ")";
    } else if (!differences.empty()) {
        out += std::format(", the first in the headers ({})", differences.front().where);
    }
    return out;
}

RelinkReport read_relink_report(const Json& j) {
    RelinkReport r;
    if (!j.is_object()) {
        r.error = "not a relink result";
        return r;
    }
    r.time = text_of(j, "time");
    r.identical = j.value("identical", false);
    for (const auto& u : array_of(j, "units")) {
        RelinkUnit unit;
        unit.unit = text_of(u, "unit");
        unit.kind = text_of(u, "kind");
        unit.mode = text_of(u, "mode");
        unit.reason = text_of(u, "reason");
        unit.object = text_of(u, "object");
        unit.bytes = number_of<u64>(u, "bytes");
        if (auto c = u.find("check"); c != u.end() && c->is_object()) unit.check = read_unit_check(*c);
        r.units.push_back(std::move(unit));
    }
    r.libraries = strings_of(j, "libraries");
    r.notes = strings_of(j, "notes");
    if (auto l = j.find("linker"); l != j.end() && l->is_object()) {
        r.linker_kind = text_of(*l, "kind");
        r.linker_version = text_of(*l, "version");
        r.linker_original = text_of(*l, "original");
        r.linker_text = text_of(*l, "text");
        if (auto same = l->find("same"); same != l->end() && same->is_boolean()) r.same_linker = same->get<bool>();
    }
    if (auto l = j.find("link"); l != j.end() && l->is_object()) {
        r.link_ok = l->value("ok", false);
        r.exit_code = number_of<int>(*l, "exit_code", -1);
        r.link_output = text_of(*l, "output");
        r.command = strings_of(*l, "command");
        r.link_ms = number_of<i64>(*l, "duration_ms");
    }
    r.image = text_of(j, "image");
    r.error = text_of(j, "error");
    if (auto c = j.find("comparison"); c != j.end() && c->is_object()) {
        r.compared = true;
        r.original_sha1 = text_of(*c, "original_sha1");
        r.relinked_sha1 = text_of(*c, "relinked_sha1");
        r.relinked_unstamped_sha1 = text_of(*c, "relinked_unstamped_sha1");
        r.original_size = number_of<u64>(*c, "original_size");
        r.relinked_size = number_of<u64>(*c, "relinked_size");
        r.differing_bytes = number_of<u64>(*c, "differing_bytes");
        for (const auto& s : array_of(*c, "stamped"))
            r.stamped.push_back({text_of(s, "name"), number_of<u32>(s, "offset"), text_of(s, "original"), text_of(s, "relinked")});
        for (const auto& d : array_of(*c, "differences")) r.differences.push_back(read_difference(d));
        for (const auto& s : array_of(*c, "sections"))
            r.sections.push_back({text_of(s, "name"), number_of<u64>(s, "differing_bytes"), rva_of(s, "first_rva"), text_of(s, "first_unit"),
                                  s.value("size_differs", false)});
        if (auto f = c->find("first"); f != c->end() && f->is_object()) r.first = read_difference(*f);
    }
    return r;
}

bool HexCompareRow::any_difference() const { return std::ranges::any_of(differs, [](bool d) { return d; }); }

std::vector<HexCompareRow> hex_compare(const pe::Image& original, const pe::Image& relinked, u32 rva, int rows_before, int rows) {
    std::vector<HexCompareRow> out;
    const i64 first_row = static_cast<i64>(rva & ~0xFu) - i64(rows_before) * 16;
    for (int r = 0; r < rows; ++r) {
        const i64 start = first_row + i64(r) * 16;
        if (start < 0) continue;
        if (start > 0xFFFFFFFF) break;
        HexCompareRow row;
        row.rva = static_cast<u32>(start);
        bool any = false;
        for (usize i = 0; i < 16; ++i) {
            row.original[i] = byte_at(original, start + static_cast<i64>(i));
            row.relinked[i] = byte_at(relinked, start + static_cast<i64>(i));
            row.differs[i] = row.original[i] != row.relinked[i];
            any = any || row.original[i] || row.relinked[i];
        }
        if (any) out.push_back(row);
    }
    return out;
}

Result<pe::Image> load_stamped_relink(const std::filesystem::path& file, const std::vector<RelinkStamped>& stamped) {
    TRY_ASSIGN(auto bytes, fs::read_file(file));
    for (const auto& s : stamped) {
        std::vector<std::byte> value;
        for (usize i = 0; i + 1 < s.original.size(); i += 2) {
            const auto hi = hex_digit(s.original[i]), lo = hex_digit(s.original[i + 1]);
            if (!hi || !lo) break;
            value.push_back(static_cast<std::byte>((*hi << 4) | *lo));
        }
        if (value.empty() || u64(s.offset) + value.size() > bytes.size()) continue;
        std::ranges::copy(value, bytes.begin() + s.offset);
    }
    return pe::Image::parse(std::move(bytes));
}

} // namespace decomp::vm
