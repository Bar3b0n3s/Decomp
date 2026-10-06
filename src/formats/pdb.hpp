#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include "formats/codeview.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace decomp::pdb {

struct Procedure {
    std::string name;  // undecorated, e.g. "Player::Hit"
    u32 rva = 0;
    u32 size = 0;
    bool global = true;  // S_GPROC32 vs S_LPROC32 (static)
    u32 module = 0;      // index into modules()
    u32 type_index = 0;  // its function type (LF_PROCEDURE, LF_MFUNCTION) in types(); 0 when unknown
};

struct DataSymbol {
    std::string name;  // undecorated
    u32 rva = 0;
    u32 type_index = 0;
    bool global = true;
    u32 module = 0;  // only meaningful for module-local (static) data
};

struct PublicSymbol {
    std::string name;  // decorated, e.g. "?Hit@Player@@QAEXH@Z"
    u32 rva = 0;
    bool is_function = false;
};

struct Module {
    std::string name;         // object file path as recorded by the linker
    std::string object_name;  // library or object name
    std::vector<std::string> source_files;  // the files its line information names: its source, then headers
    int language = -1;  // CV_CFL_* of its S_COMPILE3 record: 0 C, 1 C++, 3 MASM, 7 the linker; -1 unknown
    u16 backend_build = 0;  // the compiler's build number (19.29.30133: 30133), as its Rich header entry has it
};

struct Contribution {
    u32 rva = 0;
    u32 size = 0;
    u32 module = 0;
    u32 characteristics = 0;
};

// An input section name's range in the image (S_COFFGROUP, in the linker's module): what the linker
// merged into an image section, ".text$mn", ".xdata", ".CRT$XCU".
struct CoffGroup {
    std::string name;
    u32 rva = 0;
    u32 size = 0;
    u32 characteristics = 0;
};

struct Info {
    std::array<u8, 16> guid{};
    u32 age = 0;
    u32 signature = 0;
};

// Reads PDB 7.0 (MSF 7.00) files: procedures, data symbols, publics, modules, section contributions and
// the type records.
class Reader {
public:
    static Result<Reader> load(const std::filesystem::path& path);

    const Info& info() const { return info_; }
    const std::vector<Procedure>& procedures() const { return procedures_; }
    const std::vector<DataSymbol>& data_symbols() const { return data_; }
    const std::vector<PublicSymbol>& publics() const { return publics_; }
    const std::vector<Module>& modules() const { return modules_; }
    const std::vector<Contribution>& contributions() const { return contributions_; }
    const std::vector<CoffGroup>& coff_groups() const { return coff_groups_; }
    // The TPI stream: every type the program's code uses (empty when the PDB has none that reads).
    const codeview::TypeStream& types() const { return types_; }
    codeview::TypeStream take_types() { return std::move(types_); }
    // The file each struct, class, union and enum was defined in, by its index in types(), where the PDB
    // says (Visual C++ 8.0 and later, lld-link).
    const std::unordered_map<codeview::TypeIndex, std::string>& type_sources() const { return type_sources_; }

    // True when the GUID and age match a PE's CodeView record.
    bool matches(const std::array<u8, 16>& guid, u32 age) const { return guid == info_.guid && age == info_.age; }

private:
    Info info_;
    std::vector<Procedure> procedures_;
    std::vector<DataSymbol> data_;
    std::vector<PublicSymbol> publics_;
    std::vector<Module> modules_;
    std::vector<Contribution> contributions_;
    std::vector<CoffGroup> coff_groups_;
    codeview::TypeStream types_;
    std::unordered_map<codeview::TypeIndex, std::string> type_sources_;
};

} // namespace decomp::pdb
