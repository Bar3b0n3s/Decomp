#pragma once

// The Rich header: the record Microsoft's linker leaves between the DOS stub and the PE header of the
// tools that made the image's objects (compilers, assembler, resource compiler, linker...), as product
// id, build number and object count. The product ids are Microsoft's internal enumeration ("prodid"),
// as public research on the header documents it. This module names them, says which Visual Studio
// release each tool came with and which version a build number is, and sums up how the image was built:
// the compiler to match against (docs/architecture.md#compiler-identification).

#include "core/bytes.hpp"
#include "core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::pe {

struct RichEntry {
    u16 product_id = 0;
    u16 build = 0;
    u32 count = 0;  // objects made by the tool (imports: imported functions)
};

struct RichHeader {
    std::vector<RichEntry> entries;  // in header order
    u32 offset = 0;                  // file offset of the "DanS" marker
    u32 key = 0;                     // the XOR key, which the linker computes as a checksum
    bool checksum_ok = false;        // the key is the checksum of the DOS header and stub and the entries
};

// Finds and decodes the Rich header between the DOS header and the PE header at `pe_offset`.
std::optional<RichHeader> parse_rich_header(ByteSpan data, u32 pe_offset);
// The checksum the linker stores as the key of a header at `offset` with `entries`.
u32 rich_checksum(ByteSpan data, u32 offset, const std::vector<RichEntry>& entries);

enum class RichTool : u8 {
    unknown,
    unmarked,      // objects without a tool id (older or foreign tools)
    imports,       // imported functions
    compiler,      // C, C++, Visual Basic native code
    linker,
    assembler,     // MASM
    resources,     // cvtres
    exports,       // the export file the linker or lib.exe makes
    import_library,
    alias_object,
    cvtomf,        // OMF to COFF conversion
    pgo_converter, // cvtpgd
    ilasm,
};
std::string_view to_string(RichTool tool);

// The Visual Studio release a tool came with.
enum class VsRelease : u8 { unknown, vc5, vc6, vs2002, vs2003, vs2005, vs2008, vs2010, vs2012, vs2013, vs2015_or_later };
std::string_view to_string(VsRelease release);  // "Visual C++ 6.0", "Visual Studio .NET 2003", ...

struct RichProduct {
    u16 id = 0;
    std::string_view name;  // the enumeration's name: "Utc12_CPP", "Linker600"
    RichTool tool = RichTool::unknown;
    u8 major = 0, minor = 0;  // the tool's version: 12.00 for the VC6 compiler, 6.00 for its linker
    VsRelease release = VsRelease::unknown;
    std::string_view language;  // compilers: "C", "C++", "Basic", "MSIL"
    std::string_view variant;   // "LTCG", "PGO instrumented", "PGO optimized", "Standard edition",
                                // "Introductory edition", "CIL", "prerelease"; empty for the plain tool
};

// The product with id `id`; nullptr for an id the table does not know.
const RichProduct* rich_product(u16 id);
// "C++ compiler 12.00 (Visual C++ 6.0)", "linker 14.00 (Visual Studio 2015 or later)", or "product 0x1234".
std::string describe_rich_product(u16 id);

struct ToolVersion {
    u8 major = 0, minor = 0;
    u16 build = 0;
    bool minor_known = true;    // false for a Visual Studio 2015-or-later tool newer than the build table
    std::string visual_studio;  // "Visual C++ 6.0", "Visual Studio 2019 16.11"
    std::string suggested_name; // the toolchain name the docs use for the release: "vc6", "vs2019"
    std::string text() const;   // "12.00.8804", "19.29.30133", "19.x.36231"
};

// The version of the tool that made an entry. Visual Studio 2015 and later share product ids (compilers
// 19.xx, linkers 14.xx): their minor version and release come from the build number, or from
// `toolset_minor`, the image's linker version (14.xx), which the caller passes when the image's linker
// has the same build as the tool.
ToolVersion tool_version(const RichProduct& product, u16 build, std::optional<u8> toolset_minor = std::nullopt);

struct BuildTool {
    const RichProduct* product = nullptr;  // nullptr: an id the table does not know
    RichEntry entry;
    ToolVersion version;
    std::string description() const;  // "C++ compiler 12.00.8804", "MASM 6.14.8444", "product 0x1234 build 5"
};

// What the Rich header says about how the image was built.
struct BuildInfo {
    std::vector<BuildTool> compilers;   // most objects first
    std::optional<BuildTool> linker;
    std::vector<BuildTool> assemblers;  // most objects first
    std::vector<BuildTool> others;      // resources, exports, import libraries, unknown ids
    u32 imports = 0;                    // imported functions
    u32 unmarked = 0;                   // objects without a tool id
    bool checksum_ok = true;

    // The compiler the image's own code most likely came from: among the compilers of the linker's
    // release (or, without one, of the release with most objects), the newest build, since the runtime
    // and SDK libraries linked in were often compiled with an older one. nullptr when there is none.
    const BuildTool* main_compiler() const;
    u32 objects_in(std::string_view language) const;  // "C", "C++"
    bool has_variant(std::string_view variant) const;  // a compiler entry with this variant
};

// One tool per entry, in header order, with its version. `linker_major`/`linker_minor`: the image's
// linker version from its optional header.
std::vector<BuildTool> build_tools(const RichHeader& header, u8 linker_major, u8 linker_minor);
BuildInfo identify_build(const RichHeader& header, u8 linker_major, u8 linker_minor);

} // namespace decomp::pe
