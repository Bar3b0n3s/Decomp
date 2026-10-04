#include "analysis/program.hpp"
#include "analysis/strings.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstring>

using namespace decomp;

namespace {

const ImageString* find_text(const std::vector<ImageString>& strings, std::string_view text) {
    for (const auto& s : strings)
        if (s.text == text) return &s;
    return nullptr;
}

bool has_ref(const std::vector<Xref>& refs, u64 to, XrefKind kind) {
    return std::ranges::any_of(refs, [&](const Xref& x) { return x.to == to && x.kind == kind; });
}

// A minimal image over a byte buffer: one data section at 0x1000.
class BufferImage : public BinaryImage {
public:
    explicit BufferImage(std::vector<std::byte> data) : data_(std::move(data)) {
        sections_.push_back(ImageSection{".rdata", 0x1000, data_.size(), data_.size(), false, false, true});
        sections_.push_back(ImageSection{".text", 0x1000 + data_.size(), 0x10, 0, true, false, true});
    }
    Arch arch() const override { return Arch::x86; }
    u64 image_base() const override { return 0; }
    u64 image_size() const override { return 0x1000 + data_.size(); }
    u64 entry_point() const override { return 0; }
    const std::vector<ImageSection>& image_sections() const override { return sections_; }
    std::optional<ByteSpan> view(u64 va, usize size) const override {
        if (va < 0x1000 || va - 0x1000 + size > data_.size()) return std::nullopt;
        return ByteSpan(data_).subspan(static_cast<usize>(va - 0x1000), size);
    }
    bool has_relocations() const override { return false; }
    bool is_relocated(u64) const override { return false; }

private:
    std::vector<std::byte> data_;
    std::vector<ImageSection> sections_;
};

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

} // namespace

TEST_CASE("strings: ASCII and UTF-16LE runs, terminators, minimum length") {
    std::string data;
    data += "abc";             // too short
    data += '\0';
    data += "hello world";     // at 0x1004, terminated
    data += '\0';
    data += "\x01\x02";
    data += "tab\there\r\n";   // whitespace counts
    data += '\x7f';            // not printable: ends the run without a terminator
    data += '\0';              // pad to an even address for the wide string
    data += std::string("w\0i\0d\0e\0!\0\0\0", 12);
    data += "tail";            // runs to the end of the section
    BufferImage image(bytes_of(data));
    const auto strings = scan_strings(image);

    const ImageString* hello = find_text(strings, "hello world");
    REQUIRE(hello);
    CHECK(hello->va == 0x1004);
    CHECK(hello->encoding == StringEncoding::ascii);
    CHECK(hello->terminated);
    CHECK(hello->bytes == 11);
    CHECK(hello->section == ".rdata");
    CHECK_FALSE(find_text(strings, "abc"));
    const ImageString* tab = find_text(strings, "tab\there\r\n");
    REQUIRE(tab);
    CHECK_FALSE(tab->terminated);
    const ImageString* wide = find_text(strings, "wide!");
    REQUIRE(wide);
    CHECK(wide->encoding == StringEncoding::utf16le);
    CHECK(wide->va % 2 == 0);
    CHECK(wide->bytes == 10);
    CHECK(wide->terminated);
    const ImageString* tail = find_text(strings, "tail");
    REQUIRE(tail);
    CHECK_FALSE(tail->terminated);
    CHECK(std::ranges::is_sorted(strings, {}, &ImageString::va));

    StringScanOptions only_wide;
    only_wide.ascii = false;
    for (const auto& s : scan_strings(image, only_wide)) CHECK(s.encoding == StringEncoding::utf16le);
    StringScanOptions short_runs;
    short_runs.min_length = 3;
    CHECK(find_text(scan_strings(image, short_runs), "abc"));
    StringScanOptions capped;
    capped.max_length = 5;
    const auto pieces = scan_strings(image, capped);
    CHECK(find_text(pieces, "hello"));
    CHECK(find_text(pieces, " worl"));

    // A symbol starting inside a run cuts it.
    SymbolDb symbols;
    symbols.add(Symbol{0x100A, "w", "", "", SymbolKind::string, 0, SymbolSource::pdb, false, {}});
    const auto split = scan_strings(image, {}, &symbols);
    REQUIRE(find_text(split, "hello "));
    CHECK_FALSE(find_text(split, "hello ")->terminated);
    CHECK(find_text(split, "world"));
    CHECK(find_text(split, "world")->terminated);
    CHECK_FALSE(find_text(split, "hello world"));
}

TEST_CASE("strings: the fixtures' literals and the functions that use them") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto program = Program::open(test::fixture(std::string(arch) + "/basic.exe")).value();
        // The literal follows a floating-point constant whose last byte is '@': without symbols the two
        // run together, with them the literal starts at its symbol.
        CHECK_FALSE(find_text(scan_strings(program.image()), "hello world"));
        const auto strings = scan_strings(program.image(), {}, &program.symbols());
        const ImageString* hello = find_text(strings, "hello world");
        REQUIRE(hello);
        CHECK(program.symbols().at(hello->va)->kind == SymbolKind::string);
        CHECK(hello->terminated);
        CHECK(hello->section == ".rdata");
        const ImageString* other = find_text(strings, "other");
        REQUIRE(other);
        for (const auto& s : strings) CHECK_FALSE(program.image().is_code(s.va));

        const u64 message = *program.resolve("message");
        const u64 other_value = *program.resolve("other_value");
        const StringRefs hello_refs = string_refs(program, hello->va);
        CHECK(hello_refs.functions == std::vector<u64>{message});
        REQUIRE_FALSE(hello_refs.xrefs.empty());
        CHECK(hello_refs.xrefs[0].to == hello->va);
        CHECK(string_refs(program, other->va).functions == std::vector<u64>{other_value});
        const auto all = string_refs(program, strings);
        CHECK(all.size() == strings.size());
    }
}

TEST_CASE("xrefs_from: calls, tail jumps, reads, writes and addresses of a function") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto p = Program::open(test::fixture(std::string(arch) + "/basic.exe")).value();
        const u64 counter = *p.resolve("g_counter");
        const u64 table = *p.resolve("g_table");

        // add reads g_counter.
        const u64 add = *p.resolve("add");
        const auto add_refs = p.xrefs_from(add);
        CHECK(has_ref(add_refs, counter, XrefKind::read));
        for (const auto& x : add_refs) {
            CHECK(x.function == add);
            CHECK(p.function_extent(add)->contains(x.from));
        }

        // helper increments s_calls: a write.
        const u64 helper = *p.resolve("helper");
        const auto helper_refs = p.xrefs_from(helper);
        REQUIRE_FALSE(helper_refs.empty());
        CHECK(std::ranges::any_of(helper_refs, [](const Xref& x) { return x.kind == XrefKind::write; }));

        // dispatch calls (or tail-jumps to) add, helper and other_value, and reads g_counter and g_table.
        const u64 dispatch = *p.resolve("dispatch");
        const auto refs = p.xrefs_from(dispatch);
        auto branches_to = [&](u64 to) {
            return std::ranges::any_of(refs, [&](const Xref& x) { return x.to == to && (x.kind == XrefKind::call || x.kind == XrefKind::jump); });
        };
        CHECK(branches_to(add));
        CHECK(branches_to(helper));
        CHECK(branches_to(*p.resolve("other_value")));
        CHECK(std::ranges::any_of(refs, [&](const Xref& x) { return x.to == counter; }));
        CHECK(std::ranges::any_of(refs, [&](const Xref& x) { return x.to == table; }));
        CHECK(std::ranges::is_sorted(refs, {}, &Xref::from));

        // message takes the address of its string.
        const u64 message = *p.resolve("message");
        const auto message_refs = p.xrefs_from(message);
        REQUIRE(message_refs.size() == 1);
        CHECK(p.symbols().at(message_refs[0].to)->kind == SymbolKind::string);

        // Every outgoing reference shows up in the index from the other side.
        for (const u64 fn : {add, helper, dispatch, message})
            for (const auto& x : p.xrefs_from(fn))
                CHECK(std::ranges::any_of(p.xrefs_to(x.to), [&](const Xref& y) { return y.from == x.from && y.kind == x.kind; }));

        CHECK(p.xrefs_from(*p.resolve("g_counter")).empty());  // not code
        CHECK(to_string(XrefKind::write) == "write");
    }
}

TEST_CASE("strings: scan speed") {
    u32 state = 12345;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 24;
    };
    // Typical data: pointers, zeros and a 24-character string every 256 bytes.
    std::vector<std::byte> typical(8u << 20);
    for (usize i = 0; i < typical.size(); ++i) {
        const usize at = i % 256;
        typical[i] = static_cast<std::byte>(at < 24 ? 'a' + next() % 26 : at < 25 ? 0 : at % 4 == 3 ? 0 : next());
    }
    // Dense text: short printable runs everywhere, the worst case.
    std::vector<std::byte> dense(8u << 20);
    for (auto& b : dense) {
        const u32 r = next();
        b = static_cast<std::byte>(r < 160 ? 'a' + r % 26 : r < 200 ? 0 : r);
    }
    for (auto* data : {&typical, &dense}) {
        BufferImage image(std::move(*data));
        const auto start = std::chrono::steady_clock::now();
        const auto strings = scan_strings(image);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const std::string kind = data == &typical ? "typical data" : "dense text";
        MESSAGE("scan_strings: 8 MB of " << kind << " in " << ms << " ms, " << strings.size() << " strings");
        CHECK_FALSE(strings.empty());
        CHECK(std::ranges::is_sorted(strings, {}, &ImageString::va));
    }
}
