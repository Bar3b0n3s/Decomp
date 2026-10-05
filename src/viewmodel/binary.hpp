#pragma once

// The Binary explorer's data (docs/ui.md "Binary explorer"): section rows with decoded
// characteristics, and the overlays of the hex view (what each byte belongs to: functions, data,
// strings, floats, jump tables, relocations, import slots). Pure.

#include "analysis/program.hpp"
#include "analysis/strings.hpp"
#include "formats/pe.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct SectionRow {
    std::string name;
    u64 va = 0;
    u32 virtual_size = 0;
    u32 raw_size = 0;
    u32 raw_offset = 0;
    u32 characteristics = 0;
    std::string flags;  // "code, execute, read"
};
std::vector<SectionRow> section_rows(const pe::Image& image);
// The IMAGE_SCN_* bits that matter here, as words: "code", "initialized data", "uninitialized data",
// "discardable", "execute", "read", "write" (and "comdat" for objects).
std::string section_flags(u32 characteristics);

// What a byte range of the image is. Later kinds are drawn over earlier ones (a relocation inside a
// function shows as a relocation).
enum class OverlayKind : u8 { function, data, import, string, float_const, jump_table, relocation };
inline constexpr usize kOverlayKinds = 7;
std::string_view to_string(OverlayKind kind);  // "function", "data", "import", "string", "float", "jump table", "relocation"

struct OverlaySpan {
    u64 begin = 0, end = 0;  // [begin, end)
    OverlayKind kind = OverlayKind::data;
    std::string label;  // symbol name, string text, "dll!name", "table of 5 entries", relocation type
};

// Spans by kind, each list sorted by start, with an interval index: queries cost a binary search plus
// the spans they return (spans of one kind rarely overlap, so few are visited).
class HexOverlays {
public:
    void add(OverlaySpan span);
    // Sorts and indexes; call once after the last add().
    void finish();

    // Spans overlapping [begin, end), by kind in drawing order, then by start.
    std::vector<const OverlaySpan*> overlapping(u64 begin, u64 end) const;
    // The topmost span covering `va` (the latest kind in drawing order), if any.
    const OverlaySpan* top_at(u64 va) const;
    usize size() const;
    usize count(OverlayKind kind) const { return lists_[static_cast<usize>(kind)].spans.size(); }

private:
    struct List {
        std::vector<OverlaySpan> spans;
        std::vector<u64> max_end;  // max_end[i] = max(spans[0..i].end)
    };
    List lists_[kOverlayKinds];
};

// Functions and data from the symbols (data symbols without a size cover one byte), import slots,
// strings (from scan_strings()), float constants, jump tables (from every function's extent, which
// decodes each function: the slow part, so a background job; `cancelled` is polled between functions)
// and base relocations.
HexOverlays build_hex_overlays(const Program& program, const std::vector<ImageString>& strings, const std::function<bool()>& cancelled = {});

// "0x401000", "401000", "401000h" or a symbol name (Program::resolve) -> an address in the image.
std::optional<u64> parse_address(const Program& program, std::string_view text);

} // namespace decomp::vm
