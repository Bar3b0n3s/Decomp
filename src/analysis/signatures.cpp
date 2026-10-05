#include "analysis/signatures.hpp"

#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "formats/archive.hpp"
#include "formats/coff.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace decomp {

namespace {

// A function's bytes: up to the next function or public symbol in its section (MSVC's `$LN` labels
// inside a function are static symbols too), or the section's end.
u32 function_size(const coff::Object& obj, const coff::Symbol& sym) {
    const coff::Section* sec = obj.section(sym.section_number);
    if (!sec) return 0;
    u32 end = static_cast<u32>(sec->data.size());
    for (const coff::Symbol* other : obj.section_symbols(sym.section_number))
        if (other->value > sym.value && other->value < end && (other->is_function() || other->is_external())) end = other->value;
    return end > sym.value ? end - sym.value : 0;
}

std::optional<FunctionSignature> signature_of(const coff::Object& obj, const coff::Symbol& sym) {
    const coff::Section* sec = obj.section(sym.section_number);
    if (!sec || !sec->is_code()) return std::nullopt;
    const u32 size = function_size(obj, sym);
    if (size == 0 || sym.value + size > sec->data.size()) return std::nullopt;
    FunctionSignature s;
    s.name = sym.name;
    for (u32 i = 0; i < size; ++i) s.bytes.push_back(static_cast<u8>(sec->data[sym.value + i]));
    s.mask.assign(size, true);
    for (const auto& r : sec->relocations) {
        if (r.offset < sym.value || r.offset >= sym.value + size) continue;
        const unsigned n = obj.relocation_size(r.type);
        if (n == 0) continue;
        SignatureReference ref;
        ref.offset = r.offset - sym.value;
        ref.size = static_cast<u8>(n);
        ref.type = r.type;
        ref.pc_relative = obj.relocation_is_pc_relative(r.type);
        for (unsigned k = 0; k < n && ref.offset + k < size; ++k) s.mask[ref.offset + k] = false;
        // Public names say where the field leads; the function's own labels and sections do not.
        if (const coff::Symbol* target = obj.symbol_at_index(r.symbol_index); target && target->is_external()) ref.symbol = target->name;
        s.references.push_back(std::move(ref));
    }
    // The padding after the code belongs to no function.
    while (s.bytes.size() > 1 && s.mask.back() && (s.bytes.back() == 0xCC || s.bytes.back() == 0x90)) {
        s.bytes.pop_back();
        s.mask.pop_back();
    }
    std::erase_if(s.references, [&](const SignatureReference& r) { return r.offset >= s.bytes.size(); });
    s.significant = static_cast<usize>(std::ranges::count(s.mask, true));
    return s;
}

// The address a relocated field of the target's code leads to.
std::optional<u64> field_target(const Program& program, u64 at, const SignatureReference& ref) {
    const auto& image = program.image();
    if (ref.size == 8) return image.read<u64>(at);
    if (ref.size != 4) return std::nullopt;
    const auto raw = image.read<u32>(at);
    if (!raw) return std::nullopt;
    if (ref.pc_relative) {
        // x64 REL32_1..5 count the bytes of an immediate after the field.
        const u64 extra = image.arch() == Arch::x64 && ref.type >= coff::reloc_amd64::rel32 && ref.type <= coff::reloc_amd64::rel32_5
                              ? ref.type - coff::reloc_amd64::rel32
                              : 0;
        return at + 4 + extra + static_cast<u64>(static_cast<i64>(static_cast<i32>(*raw)));
    }
    if (image.arch() == Arch::x64 && ref.type == coff::reloc_amd64::addr32nb) return image.image_base() + *raw;
    return u64{*raw};
}

bool trusted(const Symbol& s) { return s.source != SymbolSource::analysis; }

} // namespace

Result<std::vector<FunctionSignature>> library_signatures(const std::filesystem::path& library, Arch arch, const SignatureOptions& options) {
    TRY_ASSIGN(auto lib, archive::Archive::load(library));
    std::vector<FunctionSignature> out;
    const std::string library_name = fs::to_utf8(library.filename());
    for (const auto& member : lib.members()) {
        if (member.import) continue;
        auto obj = coff::Object::parse(member.data);
        if (!obj || obj->arch() != arch) continue;  // another architecture, or not a COFF object (/GL code)
        for (const coff::Symbol* sym : obj->function_symbols()) {
            auto s = signature_of(*obj, *sym);
            if (!s || s->significant < options.min_significant) continue;
            s->member = member.name;
            s->library = library_name;
            out.push_back(std::move(*s));
        }
    }
    return out;
}

std::vector<LibraryMatch> match_library_functions(const Program& program, std::span<const FunctionSignature> signatures) {
    // Signatures by their first four bytes when those are not masked; the others are tried everywhere.
    std::unordered_map<u32, std::vector<const FunctionSignature*>> by_prefix;
    std::vector<const FunctionSignature*> irregular;
    for (const auto& s : signatures) {
        if (s.bytes.size() >= 4 && s.mask[0] && s.mask[1] && s.mask[2] && s.mask[3])
            by_prefix[u32{s.bytes[0]} | u32{s.bytes[1]} << 8 | u32{s.bytes[2]} << 16 | u32{s.bytes[3]} << 24].push_back(&s);
        else
            irregular.push_back(&s);
    }
    const auto& image = program.image();
    auto bytes_match = [&](u64 va, const FunctionSignature& s) {
        auto view = image.view(va, s.bytes.size());
        if (!view) return false;
        for (usize i = 0; i < s.bytes.size(); ++i)
            if (s.mask[i] && static_cast<u8>((*view)[i]) != s.bytes[i]) return false;
        return true;
    };

    std::map<u64, LibraryMatch> matches;
    for (const Symbol* fn : program.symbols().functions()) {
        if (!image.is_code(fn->va)) continue;
        std::vector<const FunctionSignature*> candidates;
        auto try_all = [&](const std::vector<const FunctionSignature*>& list) {
            for (const FunctionSignature* s : list)
                if (bytes_match(fn->va, *s)) candidates.push_back(s);
        };
        if (auto prefix = image.read<u32>(fn->va))
            if (auto it = by_prefix.find(*prefix); it != by_prefix.end()) try_all(it->second);
        try_all(irregular);
        // A name the image, a map or a PDB gives the function must be the signature's.
        if (trusted(*fn))
            std::erase_if(candidates, [&](const FunctionSignature* s) {
                return !names_equivalent(fn->name, s->name) && !(fn->pdb_name.size() && names_equivalent(fn->pdb_name, s->name));
            });
        if (!candidates.empty()) matches.emplace(fn->va, LibraryMatch{fn->va, std::move(candidates), nullptr});
    }

    // References settle what they can, with the names chosen so far, until nothing changes.
    std::unordered_map<u64, std::string> chosen_names;
    auto name_at = [&](u64 va) -> std::optional<std::string> {
        const u64 at = program.thunk_destination(va).value_or(va);
        if (auto it = chosen_names.find(at); it != chosen_names.end()) return it->second;
        if (const Symbol* s = program.symbols().at(at); s && trusted(*s)) return s->name;
        return std::nullopt;
    };
    auto references_fit = [&](u64 va, const FunctionSignature& s) {
        for (const auto& ref : s.references) {
            if (ref.symbol.empty()) continue;
            const auto target = field_target(program, va + ref.offset, ref);
            if (!target) continue;
            const auto name = name_at(*target);
            if (!name) continue;
            const std::string_view wanted = std::string_view(ref.symbol).starts_with("__imp_") ? std::string_view(ref.symbol).substr(6) : ref.symbol;
            const std::string_view have = std::string_view(*name).starts_with("__imp_") ? std::string_view(*name).substr(6) : *name;
            if (!names_equivalent(have, wanted)) return false;
        }
        return true;
    };
    std::set<std::string> conflicted;  // names that several functions fit
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [va, m] : matches) {
            const usize before = m.candidates.size();
            std::erase_if(m.candidates, [&](const FunctionSignature* s) { return !references_fit(va, *s); });
            if (m.candidates.size() != before) changed = true;
            // One name left (identical signatures under one name count once).
            std::set<std::string> names;
            for (const FunctionSignature* s : m.candidates) names.insert(s->name);
            const FunctionSignature* pick = names.size() == 1 && !conflicted.contains(*names.begin()) ? m.candidates.front() : nullptr;
            if (pick != m.chosen) {
                m.chosen = pick;
                changed = true;
            }
            if (m.chosen) chosen_names[va] = m.chosen->name;
            else chosen_names.erase(va);
        }
        // The linker copies a library function once: a name two functions both fit belongs to neither.
        std::map<std::string, std::vector<u64>> by_name;
        for (const auto& [va, m] : matches)
            if (m.chosen) by_name[m.chosen->name].push_back(va);
        for (const auto& [name, vas] : by_name) {
            if (vas.size() < 2) continue;
            conflicted.insert(name);
            for (u64 va : vas) {
                matches[va].chosen = nullptr;
                chosen_names.erase(va);
            }
            changed = true;
        }
    }
    std::vector<LibraryMatch> out;
    for (auto& [va, m] : matches) out.push_back(std::move(m));
    return out;
}

} // namespace decomp
