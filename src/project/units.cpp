#include "project/units.hpp"

#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/pdb.hpp"
#include "project/text_format.hpp"

#include <algorithm>
#include <cctype>
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

bool valid_unit_source(std::string_view path) {
    if (!path.starts_with("src/") || path.starts_with("src/functions/") || path.find_first_of("\\:") != std::string_view::npos) return false;
    for (usize start = 0; start <= path.size();) {
        const usize end = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) return false;
    std::string ext(path.substr(dot));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx";
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
            if (!valid_unit_source(value))
                return make_error(ErrorCode::parse, "source={} is not a C or C++ file under src/ (relative, with forward slashes)", value);
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

const Unit* source_unit(const std::vector<Unit>& units, const Symbol& fn) {
    if (fn.object.empty()) return nullptr;
    auto it = std::ranges::find(units, fn.object, &Unit::name);
    if (it == units.end() || it->kind != UnitKind::code || it->source.empty()) return nullptr;
    return &*it;
}

bool has_matched_source(const Project& project, const Symbol& fn, const std::vector<Unit>& units) {
    std::error_code ec;
    if (std::filesystem::exists(project.matched_source_path(fn), ec)) return true;
    const Unit* unit = source_unit(units, fn);
    if (!unit) return false;
    auto text = fs::read_text(project.root() / fs::from_utf8(unit->source));
    return text && matching::UnitSource::parse(*text).find(fn.va);
}

std::optional<std::filesystem::path> matched_source_location(const Project& project, const Symbol& fn, const std::vector<Unit>& units) {
    if (const Unit* unit = source_unit(units, fn)) {
        const auto path = project.root() / fs::from_utf8(unit->source);
        if (auto text = fs::read_text(path); text && matching::UnitSource::parse(*text).find(fn.va)) return path;
    }
    std::error_code ec;
    if (std::filesystem::exists(project.matched_source_path(fn), ec)) return project.matched_source_path(fn);
    return std::nullopt;
}

const Unit* matching_unit(const Project& project, const std::vector<Unit>& units, const Symbol& fn) {
    const Unit* unit = source_unit(units, fn);
    if (!unit || unit->origin != UnitOrigin::analysis) return unit;
    std::error_code ec;
    return std::filesystem::exists(project.root() / fs::from_utf8(unit->source), ec) ? unit : nullptr;
}

std::vector<std::string> verified_sources_using(const Project& project, std::string_view word) {
    auto uses = [&](std::string_view text) {
        auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
        for (usize at = text.find(word); !word.empty() && at != std::string_view::npos; at = text.find(word, at + 1)) {
            const bool starts = at == 0 || !ident(text[at - 1]);
            const bool ends = at + word.size() >= text.size() || !ident(text[at + word.size()]);
            if (starts && ends) return true;
        }
        return false;
    };
    std::vector<std::string> out;
    if (auto units = load_units(project))
        for (const auto& unit : *units) {
            if (unit.kind != UnitKind::code || unit.source.empty()) continue;
            if (auto text = fs::read_text(project.root() / fs::from_utf8(unit.source)); text && uses(*text)) out.push_back(unit.source);
        }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(project.root() / "src" / "functions", ec))
        if (entry.path().extension() == ".cpp")
            if (auto text = fs::read_text(entry.path()); text && uses(*text))
                out.push_back("src/functions/" + fs::to_utf8(entry.path().filename()));
    return out;
}

std::vector<const matching::UnitCheck*> failing_functions(const UnitChange& change, u64 except) {
    std::vector<const matching::UnitCheck*> out;
    for (const auto& check : change.verification.functions)
        if (check.va != except && !check.byte_exact()) out.push_back(&check);
    return out;
}

Result<std::vector<const matching::UnitCheck*>> broken_functions(const Program& program, const matching::MatchSetup& setup,
                                                                 const UnitChange& change, u64 except) {
    auto failing = failing_functions(change, except);
    if (failing.empty()) return failing;
    std::set<u64> exact_before;
    if (!change.base.empty()) {
        std::vector<u64> vas;
        for (const auto& f : matching::UnitSource::parse(change.base).functions) vas.push_back(f.va);
        TRY_ASSIGN(const auto base, matching::verify_unit(program, setup, change.base, change.unit.source, vas));
        for (const auto& check : base.functions)
            if (check.byte_exact()) exact_before.insert(check.va);
    }
    std::erase_if(failing, [&](const matching::UnitCheck* check) { return !exact_before.contains(check->va); });
    return failing;
}

std::string describe_checks(const Program& program, std::span<const matching::UnitCheck* const> checks) {
    std::vector<std::string> out;
    for (const auto* check : checks) {
        const Symbol* s = program.symbols().at(check->va);
        out.push_back(std::format("{} ({})", s ? qualified_name(s->name) : std::format("{:#x}", check->va),
                                  check->diff ? matching::summary_line(*check->diff) : check->error));
    }
    return join(out, "; ");
}

Result<Candidate> compile_candidate(const Project& project, const Program& program, const matching::MatchSetup& setup, const Symbol& fn,
                                    const std::string& source, const Unit* unit) {
    Candidate out;
    if (!unit) {
        TRY_ASSIGN(out.result, matching::compile_and_diff(program, setup, fn.va, source));
        return out;
    }
    TRY_ASSIGN(auto change, prepare_unit_change(project, program, setup, *unit, {{&fn, source}}));
    if (!change.rejected.empty()) {
        out.result.diff_error = change.rejected.front().second;
        out.result.compile.output = out.result.diff_error;
    } else {
        out.result.compile = change.verification.compile;
        if (!change.verification.error.empty()) out.result.diff_error = change.verification.error;
        for (const auto& check : change.verification.functions)
            if (check.va == fn.va) {
                if (check.diff) out.result.diff = check.diff;
                else out.result.diff_error = check.error;
            }
    }
    out.unit = std::move(change);
    return out;
}

Result<SavedSource> save_verified_function(const Project& project, const Program& program, const matching::MatchSetup& setup, const Symbol& fn,
                                           const std::string& source, const ChangeOrigin& origin) {
    TRY_ASSIGN(const auto units, load_units(project));
    SavedSource saved;
    const Unit* unit = matching_unit(project, units, fn);
    if (!unit) {
        TRY_ASSIGN(saved.receipt, project.write_matched_source(fn, source, origin));
        std::error_code ec;
        saved.path = std::filesystem::relative(saved.receipt.path, project.root(), ec);
        return saved;
    }
    for (int attempt = 0;; ++attempt) {
        TRY_ASSIGN(auto change, prepare_unit_change(project, program, setup, *unit, {{&fn, source}}));
        if (!change.rejected.empty()) return make_error(ErrorCode::invalid_argument, "{}", change.rejected.front().second);
        if (!change.verification.error.empty())
            return make_error(ErrorCode::invalid_argument, "{} does not compile with it: {}", unit->source, change.verification.error);
        const auto own = std::ranges::find(change.verification.functions, fn.va, &matching::UnitCheck::va);
        if (own == change.verification.functions.end() || !own->byte_exact()) {
            const matching::UnitCheck* checks[] = {own == change.verification.functions.end() ? nullptr : &*own};
            return make_error(ErrorCode::invalid_argument, "it is not byte-exact in {}{}", unit->source,
                              checks[0] ? ": " + describe_checks(program, checks) : std::string());
        }
        TRY_ASSIGN(const auto broken, broken_functions(program, setup, change, fn.va));
        if (!broken.empty())
            return make_error(ErrorCode::invalid_argument, "with it in {}, these functions are no longer byte-exact: {}", unit->source,
                              describe_checks(program, broken));
        auto receipt = commit_unit_change(project, change, origin, ChangeSubject{fn.name, fn.va, {}, {}});
        if (!receipt && receipt.error().code == ErrorCode::conflict && attempt < 3) continue;  // another writer got there first
        if (!receipt) return std::unexpected(receipt.error());
        saved.receipt = std::move(*receipt);
        saved.path = fs::from_utf8(unit->source);
        saved.unit = std::move(change);
        return saved;
    }
}

Result<UnitChange> prepare_unit_change(const Project& project, const Program& program, const matching::MatchSetup& setup, const Unit& unit,
                                       const std::vector<std::pair<const Symbol*, std::string>>& sources) {
    UnitChange change;
    change.unit = unit;
    const auto path = project.root() / fs::from_utf8(unit.source);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        TRY_ASSIGN(change.base, fs::read_text(path));
    }
    matching::UnitSource source = matching::UnitSource::parse(change.base);
    auto ordered = sources;
    std::ranges::stable_sort(ordered, {}, [](const auto& p) { return p.first->va; });
    for (const auto& [fn, text] : ordered)
        if (auto r = matching::compose_function(source, fn->va, matching::definition_names(*fn), text); !r)
            change.rejected.emplace_back(fn->va, r.error().message);
    change.content = source.render();
    for (const auto& f : source.functions) change.functions.push_back(f.va);
    if (change.rejected.size() == sources.size() && !sources.empty()) {
        change.verification.error = "nothing could be composed";  // no compile: the unit source is unchanged
        return change;
    }
    TRY_ASSIGN(change.verification, matching::verify_unit(program, setup, change.content, unit.source, change.functions));
    return change;
}

Result<WriteReceipt> commit_unit_change(const Project& project, const UnitChange& change, const ChangeOrigin& origin,
                                        const ChangeSubject& subject) {
    ChangeSubject s = subject;
    s.unit = change.unit.name;
    s.functions = change.functions;
    return project.write_project_file(fs::from_utf8(change.unit.source), change.content, origin, s, change.base);
}

Result<std::vector<UnitVerificationReport>> verify_unit_sources(const Project& project, const Program& program,
                                                               const matching::MatchSetup& setup, std::span<const std::string> names) {
    TRY_ASSIGN(const auto units, load_units(project));
    for (const auto& name : names)
        if (std::ranges::find(units, name, &Unit::name) == units.end()) return make_error(ErrorCode::not_found, "no unit named '{}'", name);
    std::vector<UnitVerificationReport> out;
    for (const auto& unit : units) {
        if (unit.kind != UnitKind::code || unit.source.empty()) continue;
        if (!names.empty() && std::ranges::find(names, unit.name) == names.end()) continue;
        const auto path = project.root() / fs::from_utf8(unit.source);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        TRY_ASSIGN(const auto text, fs::read_text(path));
        std::vector<u64> vas;
        for (const auto& f : matching::UnitSource::parse(text).functions) vas.push_back(f.va);
        UnitVerificationReport report;
        report.unit = unit;
        TRY_ASSIGN(report.verification, matching::verify_unit(program, setup, text, unit.source, vas));
        out.push_back(std::move(report));
    }
    return out;
}

Result<EmitReport> emit_unit_sources(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                     const ChangeOrigin& origin, std::span<const std::string> names) {
    TRY_ASSIGN(const auto units, load_units(project));
    for (const auto& name : names)
        if (std::ranges::find(units, name, &Unit::name) == units.end()) return make_error(ErrorCode::not_found, "no unit named '{}'", name);
    const auto infos = project.function_infos();
    std::map<std::string, std::vector<std::pair<const Symbol*, std::string>>> by_unit;
    for (const Symbol* f : program.symbols().functions()) {
        auto info = infos->find(f->va);
        if (info == infos->end() || info->second.status != FunctionStatus::matched) continue;
        const Unit* unit = source_unit(units, *f);
        if (!unit || (!names.empty() && std::ranges::find(names, unit->name) == names.end())) continue;
        auto text = fs::read_text(project.matched_source_path(*f));
        if (text) by_unit[unit->name].emplace_back(f, std::move(*text));
    }
    EmitReport report;
    for (auto& [name, sources] : by_unit) {
        const Unit& unit = *std::ranges::find(units, name, &Unit::name);
        EmitReport::UnitResult result;
        result.name = name;
        auto pending = sources;
        auto keep = [&](u64 va, std::string why) {
            result.kept.emplace_back(va, std::move(why));
            std::erase_if(pending, [&](const auto& p) { return p.first->va == va; });
        };
        // Functions that do not compose or do not stay byte-exact are left out, and the rest tried again.
        for (int round = 0; round < 8 && !pending.empty(); ++round) {
            TRY_ASSIGN(auto change, prepare_unit_change(project, program, setup, unit, pending));
            for (auto& [va, why] : change.rejected) keep(va, why);
            if (!change.verification.error.empty()) {
                std::string why = "the unit does not compile: " + change.verification.error;
                for (const auto& d : change.verification.compile.diagnostics)
                    if (d.severity.find("error") != std::string::npos) {
                        why += std::format(" ({}: {})", d.line, d.message);
                        break;
                    }
                while (!pending.empty()) keep(pending.front().first->va, why);
                break;
            }
            std::vector<std::pair<u64, std::string>> failing;
            for (const auto& check : change.verification.functions)
                if (!check.byte_exact()) failing.emplace_back(check.va, check.diff ? matching::summary_line(*check.diff) : check.error);
            if (failing.empty()) {
                if (pending.empty()) break;
                TRY(commit_unit_change(project, change, origin, ChangeSubject{}));
                for (const auto& [fn, text] : pending) {
                    std::error_code ec;
                    const auto rel = std::filesystem::relative(project.matched_source_path(*fn), project.root(), ec);
                    TRY(project.write_project_file(rel, std::nullopt, origin, ChangeSubject{fn->name, fn->va, name, {}}));
                    result.emitted.push_back(fn->va);
                }
                pending.clear();
                break;
            }
            bool existing = false;
            for (const auto& [va, why] : failing)
                if (std::ranges::find_if(pending, [&](const auto& p) { return p.first->va == va; }) == pending.end()) existing = true;
            if (existing) {
                while (!pending.empty())
                    keep(pending.front().first->va, std::format("a function already in {} is not byte-exact there; `decomp units verify` shows it",
                                                                unit.source));
                break;
            }
            for (const auto& [va, why] : failing) keep(va, "not byte-exact in the unit source: " + why);
        }
        while (!pending.empty()) keep(pending.front().first->va, "gave up after several rounds");
        report.units.push_back(std::move(result));
    }
    return report;
}

} // namespace decomp::project
