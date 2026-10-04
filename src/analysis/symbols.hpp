#pragma once

#include "core/types.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace decomp {

namespace pe { class Image; }
namespace pdb { class Reader; }

enum class SymbolKind : u8 { function, data, string, float_const, import, label, unknown };
// Ordered by trust: later sources override names from earlier ones.
enum class SymbolSource : u8 { analysis, import_table, export_table, pdb_public, pdb, agent, user };

std::string_view to_string(SymbolKind kind);
std::string_view to_string(SymbolSource source);
std::optional<SymbolKind> symbol_kind_from_string(std::string_view s);
std::optional<SymbolSource> symbol_source_from_string(std::string_view s);

struct Symbol {
    u64 va = 0;
    std::string name;      // decorated/mangled when known, else the best known name
    std::string display;   // readable (demangled) form
    std::string pdb_name;  // undecorated name from PDB procedure/data records
    SymbolKind kind = SymbolKind::unknown;
    u32 size = 0;
    SymbolSource source = SymbolSource::analysis;
    bool is_static = false;
    std::vector<std::string> aliases;  // other names at the same address (e.g. /OPT:ICF folding)
};

class SymbolDb {
public:
    // Adds or merges a symbol. At an existing address, a source of equal or higher trust takes over the
    // primary name (the previous name becomes an alias); missing size/kind/pdb_name are filled in.
    void add(Symbol symbol);
    bool remove(u64 va);
    void rename(u64 va, std::string name, SymbolSource source);

    const Symbol* at(u64 va) const;
    // Symbol whose [va, va + size) covers `address` (or the exact match when size is 0).
    const Symbol* containing(u64 address) const;
    // Lookup by decorated name, alias, PDB name, readable name or qualified name.
    const Symbol* find(std::string_view name) const;

    std::vector<const Symbol*> functions() const;
    // First function symbol strictly after `va`.
    const Symbol* next_function_after(u64 va) const;
    usize size() const { return by_va_.size(); }
    auto begin() const { return by_va_.begin(); }
    auto end() const { return by_va_.end(); }

    // Builds the database for a PE image from its export/import tables and, when given, its PDB.
    static SymbolDb from_pe(const pe::Image& image, const pdb::Reader* pdb = nullptr);

private:
    void index(const Symbol& s);
    void reindex();
    std::map<u64, Symbol> by_va_;
    std::unordered_map<std::string, u64> by_name_;
};

} // namespace decomp
