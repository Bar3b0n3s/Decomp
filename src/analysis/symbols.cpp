#include "analysis/symbols.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"
#include "formats/map.hpp"
#include "formats/pdb.hpp"
#include "formats/pe.hpp"

#include <algorithm>
#include <map>
#include <format>

namespace decomp {

std::string_view to_string(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::function: return "function";
    case SymbolKind::data: return "data";
    case SymbolKind::string: return "string";
    case SymbolKind::float_const: return "float";
    case SymbolKind::import: return "import";
    case SymbolKind::label: return "label";
    case SymbolKind::unknown: return "unknown";
    }
    return "unknown";
}

std::string_view to_string(SymbolSource source) {
    switch (source) {
    case SymbolSource::analysis: return "analysis";
    case SymbolSource::import_table: return "import";
    case SymbolSource::export_table: return "export";
    case SymbolSource::library: return "library";
    case SymbolSource::map: return "map";
    case SymbolSource::pdb_public: return "pdb_public";
    case SymbolSource::pdb: return "pdb";
    case SymbolSource::agent: return "agent";
    case SymbolSource::user: return "user";
    }
    return "analysis";
}

std::optional<SymbolKind> symbol_kind_from_string(std::string_view s) {
    for (auto k : {SymbolKind::function, SymbolKind::data, SymbolKind::string, SymbolKind::float_const,
                   SymbolKind::import, SymbolKind::label, SymbolKind::unknown})
        if (to_string(k) == s) return k;
    if (s == "func") return SymbolKind::function;
    return std::nullopt;
}

std::optional<SymbolSource> symbol_source_from_string(std::string_view s) {
    for (auto k : {SymbolSource::analysis, SymbolSource::library, SymbolSource::import_table, SymbolSource::export_table, SymbolSource::map,
                   SymbolSource::pdb_public, SymbolSource::pdb, SymbolSource::agent, SymbolSource::user})
        if (to_string(k) == s) return k;
    return std::nullopt;
}

void SymbolDb::index(const Symbol& s) {
    auto put = [&](const std::string& key) {
        if (!key.empty()) by_name_.try_emplace(key, s.va);
    };
    put(s.name);
    for (const auto& a : s.aliases) put(a);
    put(s.pdb_name);
    put(s.display);
    put(qualified_name(s.name));
}

void SymbolDb::reindex() {
    by_name_.clear();
    for (const auto& [va, s] : by_va_) index(s);
}

void SymbolDb::add(Symbol symbol) {
    if (symbol.display.empty() && !symbol.name.empty()) symbol.display = display_name(symbol.name);
    auto it = by_va_.find(symbol.va);
    if (it == by_va_.end()) {
        auto& inserted = by_va_.emplace(symbol.va, std::move(symbol)).first->second;
        index(inserted);
        return;
    }
    Symbol& existing = it->second;
    bool takes_over = symbol.source >= existing.source && !symbol.name.empty() && symbol.name != existing.name;
    if (takes_over) {
        if (!existing.name.empty() &&
            std::ranges::find(existing.aliases, existing.name) == existing.aliases.end())
            existing.aliases.push_back(existing.name);
        std::erase(existing.aliases, symbol.name);
        existing.name = symbol.name;
        existing.display = symbol.display;
        existing.source = symbol.source;
    } else if (!symbol.name.empty() && symbol.name != existing.name &&
               std::ranges::find(existing.aliases, symbol.name) == existing.aliases.end()) {
        existing.aliases.push_back(symbol.name);
    }
    if (existing.size == 0) existing.size = symbol.size;
    if (existing.pdb_name.empty()) existing.pdb_name = symbol.pdb_name;
    if (existing.object.empty()) existing.object = symbol.object;
    if (existing.kind == SymbolKind::unknown || (takes_over && symbol.kind != SymbolKind::unknown)) existing.kind = symbol.kind;
    existing.is_static = existing.is_static || symbol.is_static;
    index(existing);
}

void SymbolDb::add_alias(u64 va, const std::string& name) {
    auto it = by_va_.find(va);
    if (it == by_va_.end() || name.empty() || it->second.name == name) return;
    Symbol& s = it->second;
    if (std::ranges::find(s.aliases, name) != s.aliases.end()) return;
    s.aliases.push_back(name);
    index(s);
}

bool SymbolDb::remove(u64 va) {
    if (by_va_.erase(va) == 0) return false;
    reindex();
    return true;
}

void SymbolDb::rename(u64 va, std::string name, SymbolSource source) {
    Symbol s;
    if (auto existing = at(va)) s = *existing;
    s.va = va;
    s.name = std::move(name);
    s.display.clear();
    s.source = source;
    add(std::move(s));
    reindex();
}

const Symbol* SymbolDb::at(u64 va) const {
    auto it = by_va_.find(va);
    return it == by_va_.end() ? nullptr : &it->second;
}

const Symbol* SymbolDb::containing(u64 address) const {
    auto it = by_va_.upper_bound(address);
    if (it == by_va_.begin()) return nullptr;
    --it;
    const Symbol& s = it->second;
    if (s.va == address) return &s;
    if (s.size > 0 && address < s.va + s.size) return &s;
    return nullptr;
}

const Symbol* SymbolDb::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it != by_name_.end()) return at(it->second);
    // Fall back to equivalence on qualified names (e.g. "_ExitProcess@4" vs "ExitProcess").
    auto q = qualified_name(name);
    it = by_name_.find(q);
    return it != by_name_.end() ? at(it->second) : nullptr;
}

std::vector<const Symbol*> SymbolDb::functions() const {
    std::vector<const Symbol*> out;
    for (const auto& [va, s] : by_va_)
        if (s.kind == SymbolKind::function) out.push_back(&s);
    return out;
}

const Symbol* SymbolDb::next_function_after(u64 va) const {
    for (auto it = by_va_.upper_bound(va); it != by_va_.end(); ++it)
        if (it->second.kind == SymbolKind::function) return &it->second;
    return nullptr;
}

namespace {

struct RuntimeFunctionRange {
    u32 root = 0;  // begin of the function the entry belongs to
    u32 begin = 0;
    u32 end = 0;
};

SymbolKind kind_for_public(const pdb::PublicSymbol& p) {
    if (p.is_function) return SymbolKind::function;
    if (is_string_literal_symbol(p.name)) return SymbolKind::string;
    if (is_float_constant_symbol(p.name)) return SymbolKind::float_const;
    if (p.name.starts_with("__imp_")) return SymbolKind::import;
    return SymbolKind::data;
}

} // namespace

usize SymbolDb::add_map(const map::MapFile& m, const BinaryImage& image) {
    const u64 delta = m.preferred_base ? image.image_base() - m.preferred_base : 0;
    usize changed = 0;
    for (const auto& e : m.entries) {
        if (e.section == 0 || e.name.empty()) continue;  // absolute symbols
        const u64 va = e.va + delta;
        if (!image.contains(va)) continue;
        Symbol s;
        s.va = va;
        s.name = e.name;
        s.source = SymbolSource::map;
        s.is_static = e.is_static;
        s.object = e.object;
        if (is_string_literal_symbol(e.name)) s.kind = SymbolKind::string;
        else if (is_float_constant_symbol(e.name)) s.kind = SymbolKind::float_const;
        else if (e.name.starts_with("__imp_")) s.kind = SymbolKind::import;
        else if (!image.is_code(va)) s.kind = SymbolKind::data;
        else if ((m.has_function_flags && !e.function) || is_code_label_symbol(e.name, image.arch())) s.kind = SymbolKind::label;
        else s.kind = SymbolKind::function;
        const Symbol* before = at(va);
        if (s.kind == SymbolKind::label && before && before->kind == SymbolKind::function) {
            // A label at a function's start is another name for it; a function keeps a name from the
            // map or a better source.
            if (before->source >= SymbolSource::map) {
                add_alias(va, s.name);
                continue;
            }
            s.kind = SymbolKind::function;
        }
        const std::string old_name = before ? before->name : std::string();
        const bool existed = before != nullptr;
        add(std::move(s));
        if (!existed || at(va)->name != old_name) ++changed;
    }
    return changed;
}

SymbolDb SymbolDb::from_pe(const pe::Image& image, const pdb::Reader* pdb) {
    SymbolDb db;
    const u64 base = image.image_base();

    for (const auto& imp : image.imports()) {
        Symbol s;
        s.va = imp.iat_va;
        s.name = "__imp_" + (imp.name.empty() ? std::format("{}_{}", imp.dll, imp.ordinal.value_or(0)) : imp.name);
        s.display = std::format("{}!{}", imp.dll, imp.name.empty() ? std::format("#{}", imp.ordinal.value_or(0)) : imp.name);
        s.kind = SymbolKind::import;
        s.size = pointer_size(image.arch());
        s.source = SymbolSource::import_table;
        db.add(std::move(s));
    }
    for (const auto& e : image.exports()) {
        if (e.forwarder || e.name.empty()) continue;
        Symbol s;
        s.va = base + e.rva;
        s.name = e.name;
        s.kind = image.is_code(s.va) ? SymbolKind::function : SymbolKind::data;
        s.source = SymbolSource::export_table;
        db.add(std::move(s));
    }
    if (pdb) {
        for (const auto& p : pdb->publics()) {
            Symbol s;
            s.va = base + p.rva;
            s.name = p.name;
            s.kind = kind_for_public(p);
            s.source = SymbolSource::pdb_public;
            db.add(std::move(s));
        }
        for (const auto& p : pdb->procedures()) {
            Symbol s;
            s.va = base + p.rva;
            s.pdb_name = p.name;
            s.size = p.size;
            s.kind = SymbolKind::function;
            s.is_static = !p.global;
            s.source = SymbolSource::pdb;
            if (auto existing = db.at(s.va); existing && !existing->name.empty()) {
                s.name = existing->name;  // keep the decorated public name
                s.display = existing->display;
            } else {
                s.name = p.name;
            }
            db.add(std::move(s));
        }
        for (const auto& d : pdb->data_symbols()) {
            Symbol s;
            s.va = base + d.rva;
            s.pdb_name = d.name;
            s.kind = SymbolKind::data;
            s.is_static = !d.global;
            s.source = SymbolSource::pdb;
            if (auto existing = db.at(s.va); existing && !existing->name.empty()) {
                s.name = existing->name;
                s.display = existing->display;
                s.kind = existing->kind == SymbolKind::unknown ? SymbolKind::data : existing->kind;
            } else {
                s.name = d.name;
            }
            db.add(std::move(s));
        }
    }
    // x64 unwind data gives exact bounds for functions that have no symbol. An entry chained to another
    // is part of that function: it extends the function when it follows on directly (a function whose
    // unwind data the compiler split), and is not a function of its own otherwise (code moved away).
    std::map<u32, u32> ends;  // function begin RVA -> end RVA
    for (const auto& f : image.runtime_functions())
        if (!f.chained_to) ends.try_emplace(f.begin_rva, f.end_rva);
    std::vector<RuntimeFunctionRange> chained;
    for (const auto& f : image.runtime_functions())
        if (f.chained_to) chained.push_back({f.chained_to, f.begin_rva, f.end_rva});
    std::ranges::sort(chained, {}, &RuntimeFunctionRange::begin);
    for (const auto& c : chained)
        if (auto it = ends.find(c.root); it != ends.end() && c.begin == it->second) it->second = c.end;
    for (const auto& [begin, end] : ends) {
        const u64 va = base + begin;
        Symbol s;
        s.va = va;
        s.size = end - begin;
        s.kind = SymbolKind::function;
        s.source = SymbolSource::analysis;
        if (!db.at(va)) s.name = std::format("sub_{:x}", va);
        db.add(std::move(s));
    }
    // Data symbols from PDB records carry no size: estimate it so offsets resolve to "g_table+0x4".
    for (auto it = db.by_va_.begin(); it != db.by_va_.end(); ++it) {
        Symbol& s = it->second;
        if (s.size != 0 || s.kind == SymbolKind::function || s.kind == SymbolKind::label) continue;
        const ImageSection* section = image.section_at(s.va);
        if (!section) continue;
        u64 limit = section->va + std::max(section->virtual_size, section->file_size);
        if (auto next = std::next(it); next != db.by_va_.end()) limit = std::min(limit, next->first);
        if (s.kind == SymbolKind::string) {
            if (auto str = image.read_cstring(s.va)) limit = std::min<u64>(limit, s.va + str->size() + 1);
        } else if (s.kind == SymbolKind::float_const) {
            auto hex_digits = s.name.size() - s.name.find('@') - 1;
            limit = std::min<u64>(limit, s.va + hex_digits / 2);
        } else if (s.kind == SymbolKind::import) {
            limit = std::min<u64>(limit, s.va + pointer_size(image.arch()));
        }
        if (limit > s.va) s.size = static_cast<u32>(std::min<u64>(limit - s.va, 0xFFFFFFFFu));
    }
    if (!db.at(base)) {
        Symbol s;
        s.va = base;
        s.name = "__ImageBase";
        s.kind = SymbolKind::data;
        s.source = SymbolSource::analysis;
        db.add(std::move(s));
    }
    if (u64 entry = image.entry_point(); entry) {
        if (const Symbol* s = db.at(entry); !s) {
            Symbol e;
            e.va = entry;
            e.name = "entry";
            e.kind = SymbolKind::function;
            e.source = SymbolSource::analysis;
            db.add(std::move(e));
        } else if (s->source == SymbolSource::analysis && s->name.starts_with("sub_")) {
            db.rename(entry, "entry", SymbolSource::analysis);  // unwind data named it before the entry point was known
        }
    }
    return db;
}

} // namespace decomp
