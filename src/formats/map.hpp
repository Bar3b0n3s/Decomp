#pragma once

// Linker map files in the link.exe format (also written by lld-link /MAP): the module, the preferred load
// address, the section table, every public symbol by address with its object file, the entry point
// and the static symbols.
//
//   Address         Publics by Value              Rva+Base       Lib:Object
//  0001:00000000       _main                      00401000 f   main.obj
//  0001:00000040       _printf                    00401040 f   LIBC:printf.obj

#include "core/result.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::map {

struct Section {
    u16 section = 0;  // 1-based
    u32 offset = 0;
    u32 length = 0;
    std::string name;   // ".text", ".text$mn", ...
    std::string klass;  // "CODE", "DATA"
};

struct Entry {
    u16 section = 0;  // 1-based; 0 for absolute symbols
    u32 offset = 0;
    std::string name;    // as the linker saw it (decorated)
    u64 va = 0;          // Rva+Base: the address at the preferred load address
    bool function = false;  // flagged `f`
    bool is_static = false; // listed under "Static symbols"
    std::string object;  // "main.obj", or "LIBC:printf.obj" for a library member
};

struct MapFile {
    std::string module;
    u64 preferred_base = 0;
    std::vector<Section> sections;
    std::vector<Entry> entries;  // publics, then statics, in file order
    std::optional<std::pair<u16, u32>> entry_point;  // section:offset
    bool has_function_flags = false;  // some entry carries `f` (link.exe; lld-link writes none)
};

Result<MapFile> parse(std::string_view text);
Result<MapFile> load(const std::filesystem::path& path);

} // namespace decomp::map
