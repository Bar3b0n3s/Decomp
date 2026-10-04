#include "analysis/strings.hpp"

#include <algorithm>
#include <iterator>
#include <span>

namespace decomp {

std::string_view to_string(StringEncoding encoding) { return encoding == StringEncoding::ascii ? "ascii" : "utf16le"; }

namespace {

bool printable(unsigned c) { return (c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n' || c == '\r'; }

struct Scanner {
    const ImageSection& section;
    ByteSpan bytes;
    const StringScanOptions& options;
    std::span<const u64> splits;  // symbol starts in the section, ascending
    std::vector<ImageString>& out;

    // The character at byte offset `i`; 0x10000 past the end.
    unsigned at(usize i, usize unit) const {
        if (i + unit > bytes.size()) return 0x10000;
        const auto lo = static_cast<unsigned>(bytes[i]);
        return unit == 1 ? lo : lo | (static_cast<unsigned>(bytes[i + 1]) << 8);
    }

    void emit(usize start, usize chars, usize unit, bool terminated) {
        ImageString s;
        s.va = section.va + start;
        s.encoding = unit == 1 ? StringEncoding::ascii : StringEncoding::utf16le;
        s.bytes = static_cast<u32>(chars * unit);
        s.text.resize(chars);
        for (usize k = 0; k < chars; ++k) s.text[k] = static_cast<char>(bytes[start + k * unit]);
        s.terminated = terminated;
        s.section = section.name;
        out.push_back(std::move(s));
    }

    // Runs of `unit`-byte characters (1: ASCII, 2: UTF-16LE). Section starts are aligned, so even
    // offsets are even addresses.
    void scan(usize unit) {
        const usize n = bytes.size() - bytes.size() % unit;
        usize split = 0;
        for (usize i = 0; i < n;) {
            if (!printable(at(i, unit))) {
                i += unit;
                continue;
            }
            // The run ends at a character that is not printable, at the next symbol, or at max_length.
            const usize start = i;
            while (split < splits.size() && splits[split] <= section.va + start) ++split;
            usize limit = n;
            if (split < splits.size()) limit = std::min<usize>(n, static_cast<usize>(splits[split] - section.va + unit - 1) / unit * unit);
            limit = std::min(limit, start + options.max_length * unit);
            usize end = start + unit;
            while (end < limit && printable(at(end, unit))) end += unit;
            const usize chars = (end - start) / unit;
            if (chars >= options.min_length) emit(start, chars, unit, at(end, unit) == 0);
            i = end;
        }
    }
};

} // namespace

std::vector<ImageString> scan_strings(const BinaryImage& image, const StringScanOptions& options, const SymbolDb* symbols) {
    std::vector<ImageString> out;
    StringScanOptions effective = options;
    effective.min_length = std::max<usize>(options.min_length, 1);
    effective.max_length = std::max(options.max_length, effective.min_length);
    std::vector<u64> starts;
    if (symbols)
        for (const auto& [va, s] : *symbols) starts.push_back(va);
    std::vector<const ImageSection*> sections;
    for (const auto& section : image.image_sections())
        if (!section.executable && section.file_size > 0) sections.push_back(&section);
    std::ranges::stable_sort(sections, {}, &ImageSection::va);
    for (const ImageSection* section : sections) {
        auto bytes = image.view(section->va, static_cast<usize>(section->file_size));
        if (!bytes) continue;
        const auto first = std::ranges::lower_bound(starts, section->va);
        const auto last = std::ranges::lower_bound(starts, section->va + section->file_size);
        // Each scan yields its strings in address order; merging them keeps the whole list ordered.
        std::vector<ImageString> ascii, wide;
        if (options.ascii) Scanner{*section, *bytes, effective, std::span<const u64>(first, last), ascii}.scan(1);
        if (options.utf16le) Scanner{*section, *bytes, effective, std::span<const u64>(first, last), wide}.scan(2);
        std::merge(std::make_move_iterator(ascii.begin()), std::make_move_iterator(ascii.end()), std::make_move_iterator(wide.begin()),
                   std::make_move_iterator(wide.end()), std::back_inserter(out),
                   [](const ImageString& a, const ImageString& b) { return a.va < b.va; });
    }
    return out;
}

StringRefs string_refs(const Program& program, u64 va) {
    StringRefs refs;
    refs.xrefs = program.xrefs_to(va);
    for (const auto& x : refs.xrefs)
        if (x.function) refs.functions.push_back(x.function);
    std::ranges::sort(refs.functions);
    refs.functions.erase(std::unique(refs.functions.begin(), refs.functions.end()), refs.functions.end());
    return refs;
}

std::vector<StringRefs> string_refs(const Program& program, const std::vector<ImageString>& strings) {
    std::vector<StringRefs> out;
    out.reserve(strings.size());
    for (const auto& s : strings) out.push_back(string_refs(program, s.va));
    return out;
}

} // namespace decomp
