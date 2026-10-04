#pragma once

// Strings stored in a target image (the Binary explorer's string list and the command palette's `"`
// search), and the functions whose code references them.

#include "analysis/program.hpp"
#include "formats/image.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace decomp {

enum class StringEncoding : u8 { ascii, utf16le };
std::string_view to_string(StringEncoding encoding);  // "ascii", "utf16le"

struct ImageString {
    u64 va = 0;
    StringEncoding encoding = StringEncoding::ascii;
    std::string text;          // the characters (all ASCII by the scanning rules, so also valid UTF-8)
    u32 bytes = 0;             // size in the image, without the terminator
    bool terminated = false;   // a NUL follows (one zero byte for ASCII, a zero 16-bit unit for UTF-16LE)
    std::string section;       // name of the section holding it
};

struct StringScanOptions {
    usize min_length = 4;          // characters
    usize max_length = 64 * 1024;  // a longer run is split into strings of at most this many characters
    bool ascii = true;
    bool utf16le = true;
};

// Scans the file-backed bytes of every non-executable section (.rdata, .data, .idata, .rsrc, ...).
// A string is a run of at least `min_length` characters from the printable ASCII range 0x20-0x7E
// plus tab, line feed and carriage return:
//   ASCII:    one byte per character, at any address;
//   UTF-16LE: one 16-bit unit per character (the character, then a zero byte), starting at an even
//             address (compilers align wide strings).
// Runs need no terminator (`terminated` says whether one follows). Code sections are not scanned, and
// zero-filled (uninitialized) parts of sections hold no strings. With `symbols`, a string never runs
// across the start of a symbol: printable bytes of the data before a literal (a float constant ending
// in '@', for instance) are not glued to it when the symbol database knows where the literal starts.
// Results are ordered by address, ASCII first at equal addresses. Linear in the scanned bytes: in a
// Release build about 9 ms per MB of typical data, and up to 25 ms per MB of data dense with short
// runs of text (measured in tests/unit/analysis_strings_tests.cpp).
std::vector<ImageString> scan_strings(const BinaryImage& image, const StringScanOptions& options = {}, const SymbolDb* symbols = nullptr);

// Who references a string. The first call on a Program builds its cross-reference index
// (Program::xrefs_to), which decodes every function with a known size: about a microsecond per
// instruction in a Release build (35 to 50 ms for the 38,000 instructions of the generated program in
// tests/unit/viewmodel_eta_tests.cpp), so seconds on a large binary; call these from a background job.
// Later calls are hash lookups.
struct StringRefs {
    std::vector<Xref> xrefs;      // every instruction that references the string's first byte
    std::vector<u64> functions;   // the functions holding those instructions, unique and ascending
};
StringRefs string_refs(const Program& program, u64 va);
// The same for many strings (parallel to `strings`).
std::vector<StringRefs> string_refs(const Program& program, const std::vector<ImageString>& strings);

} // namespace decomp
