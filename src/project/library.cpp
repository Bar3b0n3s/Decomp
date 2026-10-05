#include "project/library.hpp"

#include "analysis/program.hpp"
#include "analysis/signatures.hpp"
#include "project/project.hpp"

#include <set>

namespace decomp::project {

Result<LibraryMatchReport> match_libraries(Project& project, std::span<const std::filesystem::path> libraries, bool apply) {
    TRY_ASSIGN(auto run_lock, project.try_lock_active_run());
    if (!run_lock) return make_error(ErrorCode::invalid_argument, "a run is active in this project; stop it before matching libraries");
    TRY(project.reload_if_changed());
    TRY_ASSIGN(const Program program, project.open_program());
    std::vector<FunctionSignature> signatures;
    for (const auto& lib : libraries) {
        TRY_ASSIGN(auto sigs, library_signatures(lib, program.arch()));
        for (auto& s : sigs) signatures.push_back(std::move(s));
    }
    LibraryMatchReport report;
    report.signatures = signatures.size();
    const auto matches = match_library_functions(program, signatures);

    std::map<u64, Symbol> symbols;
    for (const Symbol& s : project.symbols()) symbols.emplace(s.va, s);
    const auto infos = project.function_infos();
    auto has_work = [&](u64 va) {
        auto it = infos->find(va);
        return it != infos->end() && (it->second.status != FunctionStatus::unstarted || it->second.attempts > 0);
    };
    std::vector<u64> library_functions;
    std::set<u64> remove;
    for (const LibraryMatch& m : matches) {
        if (!m.chosen) {
            ++report.ambiguous;
            LibraryMatchReport::Ambiguous a;
            a.va = m.va;
            for (const FunctionSignature* s : m.candidates)
                if (std::ranges::find(a.names, s->name) == a.names.end()) a.names.push_back(s->name);
            report.ambiguous_functions.push_back(std::move(a));
            continue;
        }
        ++report.matched;
        ++report.by_library[m.chosen->library];
        library_functions.push_back(m.va);
        auto [it, inserted] = symbols.try_emplace(m.va);
        Symbol& s = it->second;
        s.va = m.va;
        s.kind = SymbolKind::function;
        if (inserted || s.source <= SymbolSource::library) {
            if (!s.name.empty() && s.name != m.chosen->name && !s.name.starts_with("sub_")) s.aliases.push_back(s.name);
            s.name = m.chosen->name;
            s.display.clear();
            s.source = SymbolSource::library;
        }
        const u32 size = static_cast<u32>(m.chosen->bytes.size());
        if (s.source <= SymbolSource::library || s.size == 0) {
            if (s.size != size) ++report.resized;
            s.size = size;
        }
        if (s.object.empty()) s.object = m.chosen->library + ":" + m.chosen->member;
        // Starts the analysis alone had found inside the library function were never functions.
        for (auto inner = symbols.upper_bound(m.va); inner != symbols.end() && inner->first < m.va + size; ++inner)
            if (inner->second.kind == SymbolKind::function && inner->second.source == SymbolSource::analysis && !has_work(inner->first))
                remove.insert(inner->first);
    }
    report.removed = remove.size();
    if (!apply) return report;
    SymbolDb db;
    for (auto& [va, s] : symbols)
        if (!remove.contains(va)) db.add(std::move(s));
    TRY(project.save_symbols(db));
    TRY(project.modify_functions(library_functions, [](u64, FunctionInfo& info) { info.status = FunctionStatus::library; }));
    return report;
}

Json to_json(const LibraryMatchReport& r) {
    Json ambiguous = Json::array();
    for (const auto& a : r.ambiguous_functions) ambiguous.push_back({{"va", a.va}, {"names", a.names}});
    Json libraries = Json::object();
    for (const auto& [lib, n] : r.by_library) libraries[lib] = n;
    return {{"signatures", r.signatures}, {"matched", r.matched},   {"ambiguous", r.ambiguous}, {"resized", r.resized},
            {"removed", r.removed},       {"libraries", libraries}, {"ambiguous_functions", ambiguous}};
}

} // namespace decomp::project
