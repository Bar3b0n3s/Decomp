#include "project/analyze.hpp"

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "project/project.hpp"
#include "project/units.hpp"

#include <map>

namespace decomp::project {

namespace {

bool has_work(const Project& project, const std::map<u64, FunctionInfo>& infos, const Symbol& fn) {
    if (auto it = infos.find(fn.va); it != infos.end()) {
        const FunctionInfo& info = it->second;
        if (info.status != FunctionStatus::unstarted || info.attempts > 0 || info.cost_usd > 0) return true;
    }
    std::error_code ec;
    return std::filesystem::exists(project.function_dir(fn), ec) || std::filesystem::exists(project.matched_source_path(fn), ec);
}

// Moves `from` to `to` when only `from` exists. Returns whether it moved.
Result<bool> move_if_present(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || std::filesystem::exists(to, ec)) return false;
    std::filesystem::rename(from, to, ec);
    if (ec) return make_error(ErrorCode::io, "cannot rename '{}' to '{}': {}", fs::to_utf8(from), fs::to_utf8(to), ec.message());
    return true;
}

} // namespace

Result<AnalyzeSummary> analyze(Project& project, const AnalyzeOptions& options) {
    TRY_ASSIGN(auto run_lock, project.try_lock_active_run());
    if (!run_lock) return make_error(ErrorCode::invalid_argument, "a run is active in this project; stop it before analyzing again");
    TRY(project.reload_if_changed());
    TRY(project.open_program());  // the target is still the one decomp.json describes

    std::optional<std::filesystem::path> pdb;
    if (!project.config().pdb.empty()) pdb = project.root() / fs::from_utf8(project.config().pdb);
    TRY_ASSIGN(Program fresh, Program::open(project.target_path(), OpenOptions{.pdb = pdb, .discover = false}));
    AnalyzeSummary summary;
    if (options.map) {
        TRY_ASSIGN(summary.map_symbols, fresh.add_map(*options.map));
    }

    const std::vector<Symbol> before = project.symbols();
    const auto infos = project.function_infos();
    for (const Symbol& s : before) {
        if (s.kind != SymbolKind::function) {
            fresh.symbols().add(s);
            continue;
        }
        if (s.source == SymbolSource::analysis && !has_work(project, *infos, s)) continue;  // found again, or not
        Symbol seed = s;
        // Measured again, unless a PDB or a library's own copy of the function gave the size.
        if (s.source != SymbolSource::pdb && s.source != SymbolSource::pdb_public && s.source != SymbolSource::library) seed.size = 0;
        // A name the analysis made up gives way to the one the image or the map has now.
        if (s.source == SymbolSource::analysis && fresh.symbols().at(s.va)) seed.name.clear();
        fresh.symbols().add(std::move(seed));
    }
    if (fresh.pdb_status() != PdbStatus::matched) fresh.add_discovered_functions();
    add_rtti_symbols(fresh.symbols(), fresh.rtti(), fresh.arch());

    std::map<u64, const Symbol*> old_functions;
    for (const Symbol& s : before)
        if (s.kind == SymbolKind::function) old_functions.emplace(s.va, &s);
    summary.functions_before = old_functions.size();
    const auto functions = fresh.symbols().functions();
    summary.functions = functions.size();
    for (const Symbol* f : functions) {
        auto it = old_functions.find(f->va);
        if (it == old_functions.end()) {
            ++summary.added;
            continue;
        }
        const Symbol& old = *it->second;
        old_functions.erase(it);
        if (old.size != f->size) ++summary.resized;
        if (old.name == f->name) continue;
        ++summary.renamed;
        if (safe_function_name(old) == safe_function_name(*f)) continue;
        TRY_ASSIGN(bool dir, move_if_present(project.function_dir(old), project.function_dir(*f)));
        TRY_ASSIGN(bool source, move_if_present(project.matched_source_path(old), project.matched_source_path(*f)));
        summary.moved += (dir ? 1 : 0) + (source ? 1 : 0);
    }
    summary.removed = old_functions.size();
    TRY(project.save_symbols(fresh.symbols()));
    // Units the analysis made are derived again: a map may name them now. Units from a PDB, a map or the
    // user stay (`decomp units derive --force` replaces them).
    TRY_ASSIGN(auto units, load_units(project));
    if (std::ranges::all_of(units, [](const Unit& u) { return u.origin == UnitOrigin::analysis; })) {
        TRY_ASSIGN(auto derived, derive_project_units(project, false));
        summary.units = derived.second.units;
        summary.units_from = std::string(to_string(derived.first.from));
    }
    log::debug("analysis: {} functions (was {}): {} added, {} removed, {} resized, {} renamed", summary.functions,
              summary.functions_before, summary.added, summary.removed, summary.resized, summary.renamed);
    return summary;
}

Json to_json(const AnalyzeSummary& s) {
    return {{"functions_before", s.functions_before}, {"functions", s.functions}, {"added", s.added}, {"removed", s.removed},
            {"resized", s.resized}, {"renamed", s.renamed}, {"map_symbols", s.map_symbols}, {"moved", s.moved},
            {"units", s.units}, {"units_from", s.units_from}};
}

} // namespace decomp::project
