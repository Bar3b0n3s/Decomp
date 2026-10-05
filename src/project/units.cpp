#include "project/units.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/pdb.hpp"
#include "project/text_format.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace decomp::project {

std::string format_unit_line(const Unit& unit) {
    std::string line = quote_if_needed(unit.name);
    if (unit.kind != UnitKind::code) line += std::format(" kind={}", to_string(unit.kind));
    if (!unit.source.empty()) line += " source=" + quote_if_needed(unit.source);
    if (unit.origin != UnitOrigin::user) line += std::format(" origin={}", to_string(unit.origin));
    return line;
}

Result<Unit> parse_unit_line(std::string_view line) {
    const auto tokens = tokenize(line);
    if (tokens.empty() || tokens[0].empty()) return make_error(ErrorCode::parse, "expected '<name> [kind=] [source=] [origin=]'");
    Unit u;
    u.name = tokens[0];
    u.kind = unit_kind_of(u.name);
    u.origin = UnitOrigin::user;
    for (usize i = 1; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        const auto eq = t.find('=');
        if (eq == std::string::npos) return make_error(ErrorCode::parse, "unexpected token '{}'", t);
        const std::string key = t.substr(0, eq), value = t.substr(eq + 1);
        if (key == "kind") {
            auto k = unit_kind_from_string(value);
            if (!k) return make_error(ErrorCode::parse, "unknown unit kind '{}' (code, library, import or linker)", value);
            u.kind = *k;
        } else if (key == "source") {
            u.source = value;
        } else if (key == "origin") {
            auto o = unit_origin_from_string(value);
            if (!o) return make_error(ErrorCode::parse, "unknown origin '{}' (analysis, map, pdb or user)", value);
            u.origin = *o;
        } else {
            return make_error(ErrorCode::parse, "unknown key '{}'", key);
        }
    }
    return u;
}

Result<std::vector<Unit>> load_units(const Project& project) {
    const auto path = project.root() / kUnitsFile;
    std::vector<Unit> units;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return units;
    TRY_ASSIGN(auto text, fs::read_text(path));
    usize line_no = 0;
    std::set<std::string> names;
    for (const auto& raw : split_lines(text)) {
        ++line_no;
        const auto line = trim(raw);
        if (line.empty() || line.starts_with('#')) continue;
        auto unit = parse_unit_line(line);
        if (!unit) return make_error(ErrorCode::parse, "{}:{}: {}", fs::to_utf8(path), line_no, unit.error().message);
        if (!names.insert(unit->name).second) return make_error(ErrorCode::parse, "{}:{}: unit '{}' is listed twice", fs::to_utf8(path), line_no, unit->name);
        units.push_back(std::move(*unit));
    }
    return units;
}

Result<void> save_units(const Project& project, const std::vector<Unit>& units) {
    std::string out = "# decomp units, in link order: <name> [kind=] [source=] [origin=]\n";
    for (const auto& u : units) out += format_unit_line(u) + "\n";
    return fs::write_text(project.root() / kUnitsFile, out);
}

Result<DerivedUnits> derive_units(const Program& program) {
    DerivedUnits d;
    if (program.pdb_status() == PdbStatus::matched && program.pdb_path()) {
        TRY_ASSIGN(auto reader, pdb::Reader::load(*program.pdb_path()));
        d.layout = units_from_pdb(reader, program.image(), program.symbols());
        d.from = UnitOrigin::pdb;
        return d;
    }
    const bool mapped = std::ranges::any_of(program.symbols(), [](const auto& entry) {
        return entry.second.source == SymbolSource::map && !entry.second.object.empty();
    });
    if (mapped) {
        d.layout = units_from_objects(program.symbols(), program.image());
        d.from = UnitOrigin::map;
        return d;
    }
    d.layout = units_by_analysis(program);
    d.from = UnitOrigin::analysis;
    return d;
}

Result<UnitsApplied> apply_units(Project& project, const UnitLayout& layout) {
    TRY_ASSIGN(auto existing, load_units(project));
    std::set<std::string> kept;
    for (const auto& u : existing)
        if (u.origin == UnitOrigin::user) kept.insert(u.name);
    const std::vector<Symbol> symbols = project.symbols();

    std::map<u64, std::string> objects;
    std::map<std::string, u64> first;  // unit -> its first symbol (functions before data)
    for (const auto& s : symbols) {
        std::string unit;
        if (kept.contains(s.object)) {
            unit = s.object;
        } else if (auto m = layout.members.find(s.va); m != layout.members.end()) {
            unit = m->second;
        } else if (!s.object.empty() && layout.find(s.object)) {
            unit = s.object;
        }
        if (unit != s.object) objects[s.va] = unit;
        if (unit.empty()) continue;
        const u64 key = s.kind == SymbolKind::function ? s.va : (u64{1} << 63) | s.va;
        auto [it, inserted] = first.emplace(unit, key);
        if (!inserted) it->second = std::min(it->second, key);
    }
    // The derived units in their link order, the user's among them by their first symbol.
    std::vector<Unit> units = layout.units;
    std::erase_if(units, [&](const Unit& u) { return kept.contains(u.name); });
    for (const auto& u : existing) {
        if (!kept.contains(u.name)) continue;
        const u64 key = first.contains(u.name) ? first[u.name] : ~u64{0};
        auto at = std::ranges::find_if(units, [&](const Unit& v) { return first.contains(v.name) && first[v.name] > key; });
        units.insert(at, u);
    }
    // Keep the sources the user gave units that are derived again.
    for (auto& u : units)
        for (const auto& old : existing)
            if (old.name == u.name && !old.source.empty() && u.kind == UnitKind::code && old.origin != UnitOrigin::user) u.source = old.source;
    TRY(project.assign_objects(objects));
    TRY(save_units(project, units));

    UnitsApplied applied;
    applied.units = units.size();
    for (const auto& s : symbols) {
        if (s.kind != SymbolKind::function) continue;
        const auto changed = objects.find(s.va);
        const std::string& unit = changed != objects.end() ? changed->second : s.object;
        ++(unit.empty() ? applied.unassigned : applied.functions);
    }
    return applied;
}

Result<std::pair<DerivedUnits, UnitsApplied>> derive_project_units(Project& project, bool force) {
    TRY_ASSIGN(auto existing, load_units(project));
    const bool analysis_only = std::ranges::all_of(existing, [](const Unit& u) { return u.origin == UnitOrigin::analysis; });
    if (!existing.empty() && !force && !analysis_only)
        return make_error(ErrorCode::invalid_argument, "the project has units already ({}); re-derive them with --force", kUnitsFile);
    TRY_ASSIGN(auto program, project.open_program());
    // Objects earlier derivations wrote are not records of the build: set them aside.
    std::set<std::string> derived;
    for (const auto& u : existing)
        if (u.origin == UnitOrigin::analysis || u.origin == UnitOrigin::pdb) derived.insert(u.name);
    SymbolDb symbols = program.symbols();
    for (const auto& [va, s] : program.symbols())
        if (derived.contains(s.object)) {
            Symbol copy = s;
            copy.object.clear();
            symbols.remove(va);
            symbols.add(std::move(copy));
        }
    const Program fresh = program.with_symbols(std::move(symbols));
    TRY_ASSIGN(auto units, derive_units(fresh));
    TRY_ASSIGN(auto applied, apply_units(project, units.layout));
    return std::pair{std::move(units), applied};
}

UnitLayout project_layout(const std::vector<Unit>& units, const SymbolDb& symbols) {
    UnitLayout layout;
    layout.units = units;
    for (const auto& [va, s] : symbols)
        if (!s.object.empty()) layout.members[va] = s.object;
    return layout;
}

std::vector<UnitProgress> compute_unit_progress(const std::vector<Unit>& units, const SymbolDb& symbols,
                                                const std::map<u64, FunctionInfo>& infos) {
    static const FunctionInfo kUnstarted{};
    std::vector<UnitProgress> out;
    std::map<std::string, usize, std::less<>> index;
    for (const auto& u : units) {
        index.emplace(u.name, out.size());
        UnitProgress p;
        p.unit = u;
        out.push_back(std::move(p));
    }
    std::optional<usize> unlisted;
    for (const Symbol* f : symbols.functions()) {
        usize at = 0;
        if (auto it = index.find(f->object); !f->object.empty() && it != index.end()) {
            at = it->second;
        } else {
            if (!unlisted) {
                unlisted = out.size();
                out.push_back(UnitProgress{});
            }
            at = *unlisted;
        }
        UnitProgress& p = out[at];
        const auto it = infos.find(f->va);
        const FunctionInfo& info = it != infos.end() ? it->second : kUnstarted;
        ++p.functions;
        p.bytes += f->size;
        p.cost_usd += info.cost_usd;
        ++p.statuses[info.status];
        if (info.status == FunctionStatus::matched) {
            ++p.matched;
            p.matched_bytes += f->size;
        }
    }
    return out;
}

Json to_json(const UnitProgress& p) {
    Json statuses = Json::object();
    for (const auto& [status, n] : p.statuses) statuses[std::string(to_string(status))] = n;
    Json j = {{"name", p.unit.name},
              {"kind", std::string(to_string(p.unit.kind))},
              {"origin", std::string(to_string(p.unit.origin))},
              {"functions", p.functions},
              {"matched_functions", p.matched},
              {"bytes", p.bytes},
              {"matched_bytes", p.matched_bytes},
              {"percent_functions", p.percent_functions()},
              {"percent_bytes", p.percent_bytes()},
              {"cost_usd", p.cost_usd},
              {"statuses", statuses}};
    if (!p.unit.source.empty()) j["source"] = p.unit.source;
    return j;
}

} // namespace decomp::project
