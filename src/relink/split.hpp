#pragma once

// Split objects (docs/architecture.md#relinking): COFF objects that carry the original bytes of the units
// a relink does not build from source, so the linker places everything as it did. Each contribution of a
// unit becomes a section of the same name, alignment and access; every place the image's base relocations
// cover becomes a relocation (against the import's __imp_ symbol for an IAT slot, else against
// __ImageBase with the RVA as addend), so the linker writes the same values and the same base relocations
// again. The object defines the names other objects need at their addresses.

#include "analysis/layout.hpp"
#include "core/result.hpp"
#include "formats/pe.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace decomp::relink {

struct SplitObjectSpec {
    std::vector<const Contribution*> contributions;           // what it carries, in link order
    std::map<u32, std::vector<std::string>> definitions;      // RVA -> names to define there
    std::map<u32, std::string> import_slots;                  // IAT slot RVA -> its __imp_ symbol
    std::vector<std::string> directives;                      // .drectve: /EXPORT:..., /INCLUDE:...
    std::vector<std::string> safe_seh_handlers;               // x86: names registered in .sxdata (defined here or not)
    std::optional<u32> comp_id;                               // @comp.id: the compiler link.exe counts in the Rich header
    // @feat.00: the compiler's feature bits, which link.exe counts in its VC_FEATURE debug record (the
    // compiler's generation, /GS, /sdl). Without: 1 on x86 (SAFESEH-compatible), none on x64.
    std::optional<u32> feat00;
    // C common symbols the linker allocated in .bss (name, size): declared, so it allocates them again.
    std::vector<std::pair<std::string, u32>> commons;
};

struct SplitStats {
    usize sections = 0, relocations = 0, symbols = 0;
    u64 bytes = 0;
};

// The image's __ImageBase symbol as objects name it ("___ImageBase" on x86).
std::string image_base_symbol(const pe::Image& image);
// A C name as the linker looks it up: with the underscore x86 adds ("entry" -> "_entry").
std::string c_symbol(const pe::Image& image, std::string_view name);

Result<std::vector<std::byte>> write_split_object(const pe::Image& image, const SplitObjectSpec& spec, SplitStats* stats = nullptr);

} // namespace decomp::relink
