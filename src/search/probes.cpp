#include "search/probes.hpp"

#include "core/fs.hpp"
#include "matching/source_items.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"
#include "project/units.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace decomp::search {

namespace {

std::vector<Unit> units_of(const project::Project& project) {
    auto units = project::load_units(project);
    return units ? std::move(*units) : std::vector<Unit>{};
}

} // namespace

std::vector<u64> defined_functions(const Program& program, std::string_view source) {
    std::set<std::string, std::less<>> defined;
    for (const auto& item : matching::parse_source_items(source))
        if (item.kind == matching::ItemKind::function && !item.name.empty()) defined.insert(item.name);
    std::vector<u64> out;
    for (const auto* f : program.symbols().functions())
        for (const auto& name : matching::definition_names(*f))
            if (defined.contains(name)) {
                out.push_back(f->va);
                break;
            }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Result<Probe> verified_probe(const project::Project& project, const Program& program, const Symbol& fn, bool whole) {
    (void)program;
    const auto units = units_of(project);
    const auto path = project::matched_source_location(project, fn, units);
    if (!path) return make_error(ErrorCode::not_found, "{} has no verified source", fn.display.empty() ? fn.name : fn.display);
    TRY_ASSIGN(auto text, fs::read_text(*path));
    Probe p;
    p.file_name = fs::to_utf8(path->filename());
    if (whole)
        for (const auto& f : matching::UnitSource::parse(text).functions) p.functions.push_back(f.va);
    if (p.functions.empty()) p.functions = {fn.va};
    p.source = std::move(text);
    return p;
}

std::vector<Probe> verified_probes(const project::Project& project, const Program& program) {
    const auto units = units_of(project);
    const auto infos = project.function_infos();
    // In address order of the first function of each file.
    std::vector<std::filesystem::path> order;
    std::map<std::filesystem::path, std::vector<u64>> files;
    for (const auto& [va, info] : *infos) {
        if (info.status != project::FunctionStatus::matched) continue;
        const Symbol* fn = program.symbols().at(va);
        if (!fn) continue;
        const auto path = project::matched_source_location(project, *fn, units);
        if (!path) continue;
        auto [it, added] = files.try_emplace(*path);
        if (added) order.push_back(*path);
        it->second.push_back(va);
    }
    std::vector<Probe> out;
    for (const auto& path : order) {
        auto text = fs::read_text(path);
        if (!text) continue;
        Probe p;
        p.file_name = fs::to_utf8(path.filename());
        p.functions = files[path];
        p.source = std::move(*text);
        out.push_back(std::move(p));
    }
    return out;
}

Result<Probe> unit_probe(const project::Project& project, const Unit& unit) {
    if (unit.source.empty()) return make_error(ErrorCode::not_found, "{} has no source file (units.txt source=)", unit.name);
    const auto path = project.root() / fs::from_utf8(unit.source);
    TRY_ASSIGN(auto text, fs::read_text(path));
    Probe p;
    p.file_name = fs::to_utf8(path.filename());
    for (const auto& f : matching::UnitSource::parse(text).functions) p.functions.push_back(f.va);
    if (p.functions.empty()) return make_error(ErrorCode::not_found, "{} holds no function yet", unit.source);
    p.source = std::move(text);
    return p;
}

Result<Probe> best_attempt_probe(const project::Project& project, const Symbol& fn) {
    auto source = project.best_source(fn);
    if (!source) return make_error(ErrorCode::not_found, "{} has no attempt yet", fn.display.empty() ? fn.name : fn.display);
    Probe p;
    p.source = std::move(*source);
    p.functions = {fn.va};
    // In the language of the function's unit (a C unit's attempts are C).
    const auto units = units_of(project);
    if (const Unit* unit = project::source_unit(units, fn); unit && unit->source.ends_with(".c")) p.file_name = "candidate.c";
    return p;
}

Result<Probe> file_probe(const Program& program, const std::filesystem::path& file, std::vector<u64> functions) {
    TRY_ASSIGN(auto text, fs::read_text(file));
    Probe p;
    p.file_name = fs::to_utf8(file.filename());
    p.functions = functions.empty() ? defined_functions(program, text) : std::move(functions);
    if (p.functions.empty()) return make_error(ErrorCode::not_found, "{} defines none of the target's functions", fs::to_utf8(file));
    p.source = std::move(text);
    return p;
}

} // namespace decomp::search
