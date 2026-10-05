#include "viewmodel/binary.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>

namespace decomp::vm {

std::string section_flags(u32 c) {
    std::vector<std::string> words;
    if (c & pe::scn::cnt_code) words.emplace_back("code");
    if (c & pe::scn::cnt_initialized_data) words.emplace_back("initialized data");
    if (c & pe::scn::cnt_uninitialized_data) words.emplace_back("uninitialized data");
    if (c & pe::scn::lnk_comdat) words.emplace_back("comdat");
    if (c & pe::scn::mem_discardable) words.emplace_back("discardable");
    if (c & pe::scn::mem_execute) words.emplace_back("execute");
    if (c & pe::scn::mem_read) words.emplace_back("read");
    if (c & pe::scn::mem_write) words.emplace_back("write");
    return join(words, ", ");
}

std::vector<SectionRow> section_rows(const pe::Image& image) {
    std::vector<SectionRow> out;
    for (const auto& s : image.sections()) {
        SectionRow r;
        r.name = s.name;
        r.va = image.image_base() + s.virtual_address;
        r.virtual_size = s.virtual_size;
        r.raw_size = s.raw_size;
        r.raw_offset = s.raw_offset;
        r.characteristics = s.characteristics;
        r.flags = section_flags(s.characteristics);
        out.push_back(std::move(r));
    }
    return out;
}

std::string_view to_string(OverlayKind kind) {
    switch (kind) {
    case OverlayKind::function: return "function";
    case OverlayKind::data: return "data";
    case OverlayKind::import: return "import";
    case OverlayKind::string: return "string";
    case OverlayKind::float_const: return "float";
    case OverlayKind::jump_table: return "jump table";
    case OverlayKind::relocation: return "relocation";
    }
    return "data";
}

void HexOverlays::add(OverlaySpan span) {
    if (span.end <= span.begin) span.end = span.begin + 1;
    lists_[static_cast<usize>(span.kind)].spans.push_back(std::move(span));
}

void HexOverlays::finish() {
    for (List& list : lists_) {
        std::ranges::sort(list.spans, [](const OverlaySpan& a, const OverlaySpan& b) { return a.begin != b.begin ? a.begin < b.begin : a.end < b.end; });
        list.max_end.resize(list.spans.size());
        u64 running = 0;
        for (usize i = 0; i < list.spans.size(); ++i) {
            running = std::max(running, list.spans[i].end);
            list.max_end[i] = running;
        }
    }
}

std::vector<const OverlaySpan*> HexOverlays::overlapping(u64 begin, u64 end) const {
    std::vector<const OverlaySpan*> out;
    if (end <= begin) return out;
    for (const List& list : lists_) {
        // The first span whose prefix maximum of ends passes `begin`: nothing before it can overlap.
        auto first = std::upper_bound(list.max_end.begin(), list.max_end.end(), begin);
        for (usize i = static_cast<usize>(first - list.max_end.begin()); i < list.spans.size() && list.spans[i].begin < end; ++i)
            if (list.spans[i].end > begin) out.push_back(&list.spans[i]);
    }
    return out;
}

const OverlaySpan* HexOverlays::top_at(u64 va) const {
    for (usize k = kOverlayKinds; k-- > 0;) {
        const List& list = lists_[k];
        auto first = std::upper_bound(list.max_end.begin(), list.max_end.end(), va);
        for (usize i = static_cast<usize>(first - list.max_end.begin()); i < list.spans.size() && list.spans[i].begin <= va; ++i)
            if (list.spans[i].end > va) return &list.spans[i];
    }
    return nullptr;
}

usize HexOverlays::size() const {
    usize n = 0;
    for (const List& list : lists_) n += list.spans.size();
    return n;
}

namespace {

std::string readable(const Symbol& s) { return s.display.empty() ? s.name : s.display; }

} // namespace

HexOverlays build_hex_overlays(const Program& program, const std::vector<ImageString>& strings, const std::function<bool()>& cancelled) {
    HexOverlays o;
    const pe::Image& image = program.image();
    const u64 pointer = pointer_size(image.arch());
    std::set<u64> scanned;
    for (const auto& s : strings) {
        scanned.insert(s.va);
        o.add({s.va, s.va + std::max<u64>(s.bytes, 1), OverlayKind::string, s.text});
    }
    for (const auto& imp : image.imports())
        o.add({imp.iat_va, imp.iat_va + pointer, OverlayKind::import,
               imp.name.empty() ? std::format("{}!#{}", imp.dll, imp.ordinal.value_or(0)) : std::format("{}!{}", imp.dll, imp.name)});
    std::vector<u64> functions;
    for (const auto& [va, s] : program.symbols()) {
        switch (s.kind) {
        case SymbolKind::function:
            functions.push_back(va);
            if (s.size) o.add({va, va + s.size, OverlayKind::function, readable(s)});
            break;
        case SymbolKind::data: o.add({va, va + std::max<u32>(s.size, 1), OverlayKind::data, readable(s)}); break;
        case SymbolKind::string:
            if (!scanned.contains(va)) o.add({va, va + std::max<u32>(s.size, 1), OverlayKind::string, readable(s)});
            break;
        case SymbolKind::float_const: {
            // "__real@<8 hex digits>" is a float, "__real@<16>" a double.
            const u32 size = s.size ? s.size : (s.name.size() == 7 + 16 ? 8u : 4u);
            o.add({va, va + size, OverlayKind::float_const, readable(s)});
            break;
        }
        default: break;
        }
    }
    for (const auto& r : image.base_relocations()) {
        const u64 size = r.type == 10 ? 8 : r.type == 3 ? 4 : 0;
        if (size == 0) continue;  // padding entries
        const u64 va = image.image_base() + r.rva;
        o.add({va, va + size, OverlayKind::relocation, r.type == 10 ? "DIR64" : "HIGHLOW"});
    }
    for (u64 va : functions) {
        if (cancelled && cancelled()) break;
        auto extent = program.function_extent(va);
        if (!extent) continue;
        for (const auto& t : extent->jump_tables)
            o.add({t.table_va, t.table_va + static_cast<u64>(t.entry_size) * t.targets.size(), OverlayKind::jump_table,
                   std::format("switch table of {} at {:#x}: {} entries", readable(*program.symbols().at(va)), t.jump_va, t.targets.size())});
    }
    o.finish();
    return o;
}

std::optional<u64> parse_address(const Program& program, std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    // Names first ("add" is also hex), then bare digits as hex: users type addresses as listings show them.
    std::optional<u64> va = program.resolve(text);
    if (!va || !program.image().contains(*va)) {
        va.reset();
        if (std::ranges::all_of(text, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
            va = parse_u64(std::string("0x") + std::string(text));
    }
    if (!va || !program.image().contains(*va)) return std::nullopt;
    return va;
}

} // namespace decomp::vm
