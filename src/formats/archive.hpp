#pragma once

// COFF archives (.lib): static libraries and import libraries, as lib.exe, link.exe and llvm-lib write
// them. An archive starts with "!<arch>\n"; each member has a 60-byte header (name, date, size...)
// and its data. The first two members ("/") are the linker's symbol indexes, "//" holds the long
// member names; the others are COFF objects or, in import libraries, short import objects.

#include "core/bytes.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace decomp::archive {

struct ImportObject {
    std::string symbol;  // "_ExitProcess@4"
    std::string dll;     // "KERNEL32.dll"
    u16 machine = 0;
    u16 ordinal_or_hint = 0;
    u8 type = 0;        // 0 code, 1 data, 2 const
    u8 name_type = 0;   // 0 ordinal, 1 name, 2 no prefix, 3 undecorate
};

struct Member {
    std::string name;   // "printf.obj", "D:\\build\\obj\\Release\\file.obj"
    u64 offset = 0;     // of the member's header in the archive
    std::vector<std::byte> data;
    std::optional<ImportObject> import;  // a short import object
};

class Archive {
public:
    static Result<Archive> load(const std::filesystem::path& path);
    static Result<Archive> parse(ByteSpan data);

    // The objects and import objects, in archive order (linker and name members excluded).
    const std::vector<Member>& members() const { return members_; }
    // Public symbols the linker index lists, with the member that defines each (indexes into members()).
    const std::vector<std::pair<std::string, usize>>& index() const { return index_; }

private:
    std::vector<Member> members_;
    std::vector<std::pair<std::string, usize>> index_;
};

} // namespace decomp::archive
