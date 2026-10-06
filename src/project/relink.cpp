#include "project/relink.hpp"

#include "analysis/units.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"
#include "formats/archive.hpp"
#include "formats/coff_writer.hpp"
#include "formats/rich.hpp"
#include "matching/unit_source.hpp"
#include "project/units.hpp"
#include "relink/split.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace decomp::project {

Result<ImageLayout> project_image_layout(const Program& program, const std::vector<Unit>& units) {
    if (program.pdb_status() == PdbStatus::matched && program.pdb_path()) {
        TRY_ASSIGN(auto reader, pdb::Reader::load(*program.pdb_path()));
        return layout_from_pdb(reader, program.image());
    }
    return layout_from_units(program, project_layout(units, program.symbols()));
}

std::string UnitSourceCheck::summary() const {
    std::vector<std::string> parts;
    if (!error.empty()) parts.push_back(error);
    // An incomplete unit's check fails for the missing functions alone: say that.
    if (!missing_functions.empty())
        parts.push_back(std::format("{} of {} functions in its source", functions.size() - missing_functions.size(), functions.size()));
    else if (check && !check->ok())
        parts.push_back(check->summary());
    return parts.empty() ? "complete" : join(parts, "; ");
}

namespace {

std::vector<u64> unit_functions(const Program& program, const Unit& unit) {
    std::vector<u64> out;
    for (const Symbol* f : program.symbols().functions())
        if (f->object == unit.name) out.push_back(f->va);
    return out;
}

Result<UnitSourceCheck> check_unit_text(const Program& program, const matching::MatchSetup& setup, const ImageLayout& layout,
                                        const Unit& unit, const std::string& text) {
    UnitSourceCheck out;
    out.unit = unit;
    out.functions = unit_functions(program, unit);
    const auto source = matching::UnitSource::parse(text);
    std::set<u64> held;
    for (const auto& f : source.functions) held.insert(f.va);
    for (u64 va : out.functions)
        if (!held.contains(va)) out.missing_functions.push_back(va);

    matching::Compiler compiler(setup.toolchain, setup.work_dir, setup.cache_dir);
    matching::CompileRequest req;
    req.source = text;
    req.flags = setup.flags;
    req.include_dirs = setup.include_dirs;
    req.cancelled = setup.cancelled;
    req.bypass_cache = setup.bypass_cache;
    req.file_name = fs::to_utf8(fs::from_utf8(unit.source).filename());
    TRY_ASSIGN(out.compile, compiler.compile(req));
    if (!out.compile.ok) {
        out.error = out.compile.cancelled ? "the compile was cancelled" : out.compile.timed_out ? "the compiler timed out" : "the compile failed";
        return out;
    }
    auto object = coff::Object::parse(out.compile.object_data);
    if (!object) {
        out.error = "cannot read the compiled object: " + object.error().message;
        return out;
    }
    out.check = matching::check_unit(program, layout, *object, unit.name, std::vector<u64>(held.begin(), held.end()));
    return out;
}

} // namespace

Result<UnitSourceCheck> check_unit_source(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                          const ImageLayout& layout, const Unit& unit) {
    const auto path = project.root() / fs::from_utf8(unit.source);
    std::error_code ec;
    if (unit.source.empty() || !std::filesystem::exists(path, ec)) {
        UnitSourceCheck out;
        out.unit = unit;
        out.functions = unit_functions(program, unit);
        out.error = unit.source.empty() ? "no source" : "no source file " + unit.source;
        out.missing_functions = out.functions;
        return out;
    }
    TRY_ASSIGN(const auto text, fs::read_text(path));
    return check_unit_text(program, setup, layout, unit, text);
}

Result<ComposedUnit> compose_unit_source(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                         const std::string& unit_name, std::string_view source) {
    TRY_ASSIGN(const auto units, load_units(project));
    auto unit = std::ranges::find(units, unit_name, &Unit::name);
    if (unit == units.end()) return make_error(ErrorCode::not_found, "no unit named '{}'", unit_name);
    if (unit->kind != UnitKind::code || unit->source.empty())
        return make_error(ErrorCode::invalid_argument, "{} is not a code unit with a source file (units.txt source=)", unit_name);
    ComposedUnit out;
    out.unit = *unit;
    matching::UnitSource composed;
    for (u64 va : unit_functions(program, *unit)) {
        const auto* f = program.symbols().at(va);
        if (auto r = matching::compose_function(composed, va, matching::definition_names(*f), source); !r)
            out.rejected.emplace_back(va, r.error().message);
    }
    out.content = composed.render();
    TRY_ASSIGN(const auto layout, project_image_layout(program, units));
    TRY_ASSIGN(out.check, check_unit_text(program, setup, layout, *unit, out.content));
    return out;
}

Result<std::vector<UnitSourceCheck>> check_unit_sources(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                                        std::span<const std::string> names) {
    TRY_ASSIGN(const auto units, load_units(project));
    for (const auto& name : names)
        if (std::ranges::find(units, name, &Unit::name) == units.end()) return make_error(ErrorCode::not_found, "no unit named '{}'", name);
    TRY_ASSIGN(const auto layout, project_image_layout(program, units));
    std::vector<UnitSourceCheck> out;
    for (const auto& unit : units) {
        if (unit.kind != UnitKind::code) continue;
        if (!names.empty() && std::ranges::find(names, unit.name) == names.end()) continue;
        std::error_code ec;
        if (names.empty() && (unit.source.empty() || !std::filesystem::exists(project.root() / fs::from_utf8(unit.source), ec))) continue;
        TRY_ASSIGN(auto check, check_unit_source(project, program, setup, layout, unit));
        out.push_back(std::move(check));
    }
    return out;
}

Json to_json(const UnitSourceCheck& c) {
    Json missing = Json::array();
    for (u64 va : c.missing_functions) missing.push_back(va);
    Json j{{"unit", c.unit.name},
           {"source", c.unit.source},
           {"complete", c.complete()},
           {"summary", c.summary()},
           {"functions", c.functions.size()},
           {"missing_functions", std::move(missing)}};
    if (!c.error.empty()) j["error"] = c.error;
    if (!c.compile.ok && !c.compile.output.empty()) j["compile_output"] = c.compile.output;
    if (c.check) j["check"] = matching::to_json(*c.check);
    return j;
}

std::string_view to_string(LinkMode mode) {
    switch (mode) {
    case LinkMode::source: return "source";
    case LinkMode::split: return "split";
    case LinkMode::linker: return "linker";
    }
    return "?";
}

usize RelinkResult::count(LinkMode mode) const {
    return static_cast<usize>(std::ranges::count_if(units, [&](const UnitLink& u) { return u.mode == mode; }));
}

std::filesystem::path relink_dir(const Project& project) { return project.root() / ".decomp" / "relink"; }

namespace {

std::string now_utc() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string file_part(std::string_view unit) {
    std::string out;
    for (char c : unit) out += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_';
    if (out.ends_with(".obj")) out.resize(out.size() - 4);
    return out.empty() ? std::string("unit") : out;
}

bool is_image_base(std::string_view name) { return name == "__ImageBase" || name == "___ImageBase"; }

// The exports a compiled object's directives ask for, by the symbols they name.
std::vector<std::string> exported_symbols(const coff::Object& object) {
    std::vector<std::string> out;
    for (const auto& s : object.sections()) {
        if (s.name != ".drectve") continue;
        const std::string text(reinterpret_cast<const char*>(s.data.data()), s.data.size());
        for (auto token : split(text, ' ')) {
            token = trim(token);
            const auto lower = to_lower(token);
            if (!lower.starts_with("/export:") && !lower.starts_with("-export:")) continue;
            std::string_view spec = token.substr(8);
            if (spec.starts_with('"')) spec = spec.substr(1, spec.find('"', 1) - 1);
            spec = spec.substr(0, spec.find(','));
            if (auto eq = spec.find('='); eq != std::string_view::npos) spec = spec.substr(eq + 1);
            out.emplace_back(spec);
        }
    }
    return out;
}

struct ImportInfo {
    std::string dll;
    std::string name;
    std::optional<u16> ordinal;
    u16 hint = 0;
    u32 slot = 0;
    std::optional<u32> thunk;
    std::string symbol;  // without __imp_
};

} // namespace

Result<RelinkResult> relink_project(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                    const RelinkOptions& options) {
    RelinkResult result;
    result.time = now_utc();
    auto progress = [&](const std::string& line) {
        if (options.progress) options.progress(line);
    };
    auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    const auto& image = program.image();
    const u64 base = image.image_base();
    const bool x86 = image.machine() == pe::machine::i386;

    TRY_ASSIGN(const auto units, load_units(project));
    if (units.empty()) return make_error(ErrorCode::invalid_argument, "the project has no units (units.txt): run decomp units derive first");
    for (const auto& name : options.source)
        if (std::ranges::find(units, name, &Unit::name) == units.end()) return make_error(ErrorCode::not_found, "no unit named '{}'", name);
    for (const auto& name : options.split)
        if (std::ranges::find(units, name, &Unit::name) == units.end()) return make_error(ErrorCode::not_found, "no unit named '{}'", name);
    TRY_ASSIGN(const auto layout, project_image_layout(program, units));

    const auto dir = relink_dir(project);
    std::error_code ec;
    std::filesystem::remove_all(dir / "objects", ec);
    std::filesystem::remove_all(dir / "libs", ec);
    std::filesystem::remove_all(dir / "out", ec);
    TRY(fs::create_directories(dir / "objects"));
    TRY(fs::create_directories(dir / "libs"));
    TRY(fs::create_directories(dir / "out"));

    // Which units come from their sources.
    std::map<std::string, usize> link_index;
    for (const auto& unit : units) {
        if (cancelled()) return make_error(ErrorCode::cancelled, "the relink was cancelled");
        UnitLink link;
        link.unit = unit;
        for (const auto* c : layout.of_unit(unit.name))
            if (!c->linker) link.bytes += c->size;
        const bool forced_source = std::ranges::find(options.source, unit.name) != options.source.end();
        const bool forced_split = options.all_split || std::ranges::find(options.split, unit.name) != options.split.end();
        if (unit.kind == UnitKind::import || unit.kind == UnitKind::linker) {
            link.mode = LinkMode::linker;
            link.reason = unit.kind == UnitKind::import ? "made by the linker from the import libraries" : "made by the linker";
        } else if (unit.kind == UnitKind::code && !unit.source.empty() && std::filesystem::exists(project.root() / fs::from_utf8(unit.source), ec)) {
            progress(std::format("checking {} ({})", unit.name, unit.source));
            TRY_ASSIGN(auto check, check_unit_source(project, program, setup, layout, unit));
            if (forced_split) {
                link.mode = LinkMode::split;
                link.reason = options.all_split ? "every unit split" : "split on request";
            } else if (check.complete() || (forced_source && check.error.empty())) {
                link.mode = LinkMode::source;
                link.reason = check.complete() ? "complete" : "from its source on request: " + check.summary();
            } else {
                link.mode = LinkMode::split;
                link.reason = check.summary();
            }
            link.check = std::move(check);
        } else {
            link.mode = LinkMode::split;
            link.reason = unit.kind == UnitKind::library ? "a library's member" : "no source";
        }
        link_index[unit.name] = result.units.size();
        result.units.push_back(std::move(link));
    }
    // Contributions of units units.txt does not list are carried after the listed ones.
    for (const auto& c : layout.contributions) {
        if (c.linker || link_index.contains(c.unit)) continue;
        UnitLink link;
        link.unit = Unit{c.unit, unit_kind_of(c.unit), {}, UnitOrigin::analysis};
        link.mode = LinkMode::split;
        link.reason = c.unit.empty() ? "bytes no unit holds" : "not in units.txt";
        for (const auto* d : layout.of_unit(c.unit))
            if (!d->linker) link.bytes += d->size;
        link_index[c.unit] = result.units.size();
        result.units.push_back(std::move(link));
        result.notes.push_back(std::format("{}: carried after the listed units", c.unit.empty() ? "(no unit)" : c.unit));
    }
    auto mode_of = [&](const std::string& unit) {
        auto it = link_index.find(unit);
        return it == link_index.end() ? LinkMode::split : result.units[it->second].mode;
    };
    auto source_check = [&](const std::string& unit) -> const matching::UnitCheckResult* {
        auto it = link_index.find(unit);
        if (it == link_index.end()) return nullptr;
        const auto& u = result.units[it->second];
        return u.mode == LinkMode::source && u.check && u.check->check ? &*u.check->check : nullptr;
    };

    // Imports: one per IAT slot, with the symbol objects use for it.
    std::vector<ImportInfo> imports;
    std::map<u32, usize> import_by_slot, import_by_thunk;
    for (const auto& imp : image.imports()) {
        ImportInfo info;
        info.dll = imp.dll;
        info.name = imp.name;
        info.ordinal = imp.ordinal;
        info.hint = imp.hint;
        info.slot = static_cast<u32>(imp.iat_va - base);
        info.symbol = imp.ordinal ? std::format("{}decomp_{}_{}", x86 ? "_" : "", file_part(imp.dll), *imp.ordinal) : relink::c_symbol(image, imp.name);
        if (const auto* s = program.symbols().at(imp.iat_va)) {
            for (const auto* n : {&s->name}) {
                if (n->starts_with("__imp_")) info.symbol = n->substr(6);
            }
            for (const auto& alias : s->aliases)
                if (alias.starts_with("__imp_")) info.symbol = alias.substr(6);
        }
        import_by_slot[info.slot] = imports.size();
        imports.push_back(std::move(info));
    }
    for (const auto& [va, s] : program.symbols()) {
        if (!image.is_code(va)) continue;
        if (auto to = program.thunk_destination(va)) {
            const u32 slot = static_cast<u32>(*to - base);
            if (auto it = import_by_slot.find(slot); it != import_by_slot.end() && !imports[it->second].thunk) {
                imports[it->second].thunk = static_cast<u32>(va - base);
                import_by_thunk[static_cast<u32>(va - base)] = it->second;
            }
        }
    }

    // What each split unit defines: the names compiled objects reference there, the names of its
    // COMDATs (a compiled unit's copy of a pooled one is then the one the linker drops), the entry point,
    // the exports and the symbols the linker looks up.
    std::map<std::string, std::map<u32, std::vector<std::string>>> definitions;  // unit -> rva -> names
    std::vector<std::string> directives;
    auto define = [&](const std::string& unit, u32 rva, const std::string& name) {
        auto& names = definitions[unit][rva];
        if (std::ranges::find(names, name) == names.end()) names.push_back(name);
    };
    auto owner = [&](u32 rva) -> const Contribution* { return layout.at(rva); };
    for (const auto& link : result.units) {
        const auto* check = link.mode == LinkMode::source && link.check && link.check->check ? &*link.check->check : nullptr;
        if (!check) continue;
        for (const auto& [name, rva] : check->externals) {
            if (is_image_base(name)) continue;
            if (auto it = import_by_slot.find(rva); it != import_by_slot.end() && name.starts_with("__imp_")) {
                imports[it->second].symbol = name.substr(6);
                continue;
            }
            if (auto it = import_by_thunk.find(rva); it != import_by_thunk.end()) {
                imports[it->second].symbol = name;
                continue;
            }
            const auto* c = owner(rva);
            if (!c) {
                result.notes.push_back(std::format("{} references {} at {:#x}, which no contribution holds", link.unit.name, name, rva));
                continue;
            }
            if (c->linker) continue;
            if (mode_of(c->unit) != LinkMode::source) {
                define(c->unit, rva, name);
                continue;
            }
            const auto* theirs = source_check(c->unit);
            if (!theirs) continue;
            if (auto d = theirs->definitions.find(name); d != theirs->definitions.end() && d->second == rva) continue;
            auto same = std::ranges::find_if(theirs->definitions, [&](const auto& d) { return d.second == rva; });
            if (same != theirs->definitions.end()) directives.push_back(std::format("/ALTERNATENAME:{}={}", name, same->first));
            else result.notes.push_back(std::format("{} references {} at {:#x}, which {}'s object does not define", link.unit.name, name, rva, c->unit));
        }
        for (const auto& s : check->sections)
            if (s.state == matching::PlacementState::discarded && s.rva && !s.symbol.empty())
                if (const auto* c = owner(*s.rva); c && !c->linker && mode_of(c->unit) == LinkMode::split) define(c->unit, *s.rva, s.symbol);
    }
    if (layout.source == LayoutSource::pdb) {
        for (const auto& c : layout.contributions) {
            if (c.linker || !(c.characteristics & pe::scn::lnk_comdat) || mode_of(c.unit) != LinkMode::split) continue;
            const auto* s = program.symbols().at(base + c.rva);
            if (!s || s->is_static || s->name.empty() || s->source < SymbolSource::pdb_public) continue;
            define(c.unit, c.rva, s->name);
        }
    }
    // The entry point.
    std::string entry;
    if (const u32 rva = image.entry_rva()) {
        const auto* c = owner(rva);
        if (c && !c->linker && mode_of(c->unit) == LinkMode::split) {
            define(c->unit, rva, relink::c_symbol(image, "decomp_entry"));
            entry = "decomp_entry";
        } else if (const auto* theirs = c ? source_check(c->unit) : nullptr) {
            auto it = std::ranges::find_if(theirs->definitions, [&](const auto& d) { return d.second == rva; });
            if (it != theirs->definitions.end()) entry = x86 && it->first.starts_with('_') ? it->first.substr(1) : it->first;
        }
        if (entry.empty()) result.notes.push_back(std::format("no symbol for the entry point at {:#x}", rva));
    }
    // Exports the compiled objects do not make themselves.
    std::set<u32> exported_by_objects;
    for (const auto& link : result.units) {
        if (link.mode != LinkMode::source || !link.check) continue;
        auto object = coff::Object::parse(link.check->compile.object_data);
        if (!object || !link.check->check) continue;
        for (const auto& name : exported_symbols(*object))
            if (auto d = link.check->check->definitions.find(name); d != link.check->check->definitions.end()) exported_by_objects.insert(d->second);
    }
    for (const auto& e : image.exports()) {
        const std::string exported = e.name.empty() ? std::format("decomp_noname_{}", e.ordinal) : e.name;
        const std::string suffix = std::format(",@{}{}", e.ordinal, e.name.empty() ? ",NONAME" : "");
        if (e.forwarder) {
            directives.push_back(std::format("/EXPORT:{}={}{}", exported, *e.forwarder, suffix));
            continue;
        }
        if (exported_by_objects.contains(e.rva)) continue;
        const auto* c = owner(e.rva);
        if (c && !c->linker && mode_of(c->unit) == LinkMode::split) {
            const auto symbol = relink::c_symbol(image, std::format("decomp_export_{}", e.ordinal));
            define(c->unit, e.rva, symbol);
            directives.push_back(std::format("/EXPORT:{}={}{}", exported, symbol, suffix));
        } else if (const auto* theirs = c ? source_check(c->unit) : nullptr) {
            auto it = std::ranges::find_if(theirs->definitions, [&](const auto& d) { return d.second == e.rva; });
            if (it != theirs->definitions.end()) directives.push_back(std::format("/EXPORT:{}={}{}", exported, it->first, suffix));
            else result.notes.push_back(std::format("no symbol for the export {} at {:#x}", exported, e.rva));
        } else {
            result.notes.push_back(std::format("no object holds the export {} at {:#x}", exported, e.rva));
        }
    }
    // Symbols the linker looks up for the TLS and load configuration directories.
    for (auto [index, name] : {std::pair<u32, const char*>{9, "_tls_used"}, {10, "_load_config_used"}}) {
        const u32 rva = image.data_directory(index).first;
        if (!rva) continue;
        if (const auto* c = owner(rva); c && !c->linker && mode_of(c->unit) == LinkMode::split) define(c->unit, rva, relink::c_symbol(image, name));
    }
    // x86 /SAFESEH: the linker's table of exception handlers is made from what the objects register,
    // so split objects register the handlers they hold (and those of compiled units, which is harmless).
    std::map<std::string, std::vector<std::string>> seh_by_unit;
    std::vector<std::string> seh_elsewhere;
    if (x86) {
        std::vector<u32> handlers = image.safe_seh_handlers();
        // Without a load configuration lld-link still writes the table; the PDB names it.
        if (const auto* t = handlers.empty() ? program.symbols().find("___safe_se_handler_table") : nullptr; t && t->va > base) {
            const u32 table = static_cast<u32>(t->va - base);
            if (const auto* c = layout.at(table); c && c->linker) {
                for (u32 at = table; at + 4 <= c->end(); at += 4) {
                    auto v = image.read_rva(at, 4);
                    const u32 rva = v ? read_le<u32>(*v, 0).value_or(0) : 0;
                    const auto* s = image.section_for_rva(rva);
                    if (!s || !(s->characteristics & pe::scn::mem_execute)) break;
                    handlers.push_back(rva);
                }
            }
        }
        for (u32 rva : handlers) {
            const auto* c = owner(rva);
            if (c && !c->linker && mode_of(c->unit) == LinkMode::split) {
                const auto name = std::format("$decomp_seh_{:x}", rva);
                define(c->unit, rva, name);
                seh_by_unit[c->unit].push_back(name);
            } else if (const auto* theirs = c ? source_check(c->unit) : nullptr) {
                auto it = std::ranges::find_if(theirs->definitions, [&](const auto& d) { return d.second == rva; });
                if (it != theirs->definitions.end()) seh_elsewhere.push_back(it->first);
            }
        }
    }
    // Every import is pulled in: split code reaches some only through bytes the linker does not read.
    std::map<u32, std::string> import_slots;
    for (const auto& imp : imports) {
        import_slots[imp.slot] = "__imp_" + imp.symbol;
        directives.push_back("/INCLUDE:__imp_" + imp.symbol);
        if (imp.thunk) directives.push_back("/INCLUDE:" + imp.symbol);
    }

    // link.exe counts each object's @comp.id in the Rich header: a split object carries its original's,
    // the header's entry for the compiler and build the PDB's compile record names.
    auto rich_id = [&](auto&& fits, std::optional<u16> build) -> std::optional<u32> {
        for (bool plain : {true, false})
            for (const auto& e : image.rich_entries()) {
                const auto* product = pe::rich_product(e.product_id);
                if (!product || (build && e.build != *build) || product->variant.empty() != plain || !fits(*product)) continue;
                return (static_cast<u32>(e.product_id) << 16) | e.build;
            }
        return std::nullopt;
    };
    auto comp_id_for = [&](const std::string& unit) -> std::optional<u32> {
        auto o = layout.origins.find(unit);
        if (o == layout.origins.end()) return std::nullopt;
        const int language = o->second.language;
        return rich_id(
            [&](const pe::RichProduct& p) {
                if (language == 0) return p.tool == pe::RichTool::compiler && p.language == "C";
                if (language == 1) return p.tool == pe::RichTool::compiler && p.language == "C++";
                if (language == 3) return p.tool == pe::RichTool::assembler;
                if (language == 8) return p.tool == pe::RichTool::resources;
                return false;
            },
            o->second.build);
    };
    const auto import_library_id = rich_id([](const pe::RichProduct& p) { return p.tool == pe::RichTool::import_library; }, std::nullopt);

    // The objects, in link order.
    std::vector<std::string> inputs;
    bool directives_placed = false;
    usize index = 0;
    for (auto& link : result.units) {
        if (cancelled()) return make_error(ErrorCode::cancelled, "the relink was cancelled");
        const auto file = std::format("objects/{:03}_{}.obj", index++, file_part(link.unit.name));
        if (link.mode == LinkMode::source) {
            TRY(fs::write_file(dir / fs::from_utf8(file), link.check->compile.object_data));
        } else if (link.mode == LinkMode::split) {
            relink::SplitObjectSpec spec;
            for (const auto* c : layout.of_unit(link.unit.name))
                if (!c->linker) spec.contributions.push_back(c);
            if (spec.contributions.empty() && directives_placed) continue;
            if (auto d = definitions.find(link.unit.name); d != definitions.end()) spec.definitions = d->second;
            spec.import_slots = import_slots;
            if (auto h = seh_by_unit.find(link.unit.name); h != seh_by_unit.end()) spec.safe_seh_handlers = h->second;
            spec.comp_id = comp_id_for(link.unit.name);
            if (!directives_placed) {
                spec.directives = directives;
                spec.safe_seh_handlers.insert(spec.safe_seh_handlers.end(), seh_elsewhere.begin(), seh_elsewhere.end());
                directives_placed = true;
            }
            relink::SplitStats stats;
            TRY_ASSIGN(auto bytes, relink::write_split_object(image, spec, &stats));
            TRY(fs::write_file(dir / fs::from_utf8(file), bytes));
            progress(std::format("split {}: {} sections, {} relocations", link.unit.name, stats.sections, stats.relocations));
        } else {
            continue;
        }
        link.object = file;
        inputs.push_back(fs::to_utf8(dir / fs::from_utf8(file)));
    }
    if (!directives_placed && !directives.empty()) {
        // Everything is compiled: the directives go in an object of their own, first.
        relink::SplitObjectSpec spec;
        spec.directives = directives;
        TRY_ASSIGN(auto bytes, relink::write_split_object(image, spec));
        TRY(fs::write_file(dir / "objects" / "directives.obj", bytes));
        inputs.insert(inputs.begin(), fs::to_utf8(dir / "objects" / "directives.obj"));
    }

    // Libraries: the configured ones, then import libraries for the DLLs they do not cover.
    std::set<std::string> covered;
    for (const auto& lib : project.config().link.libraries) {
        auto path = fs::from_utf8(lib);
        // A bare name not in the project ("kernel32.lib") is for the linker to find (LIB).
        if (path.is_relative() && (path.has_parent_path() || std::filesystem::exists(project.root() / path, ec))) path = project.root() / path;
        inputs.push_back(fs::to_utf8(path));
        // Which DLLs it covers: a bare name is read where the linker finds it.
        if (!std::filesystem::exists(path, ec))
            if (auto dirs = get_env("LIB"))
                for (auto dir : split(*dirs, path_list_separator()))
                    if (std::filesystem::exists(fs::from_utf8(std::string(trim(dir))) / path, ec)) {
                        path = fs::from_utf8(std::string(trim(dir))) / path;
                        break;
                    }
        if (auto archive = archive::Archive::load(path))
            for (const auto& m : archive->members())
                if (m.import) covered.insert(to_lower(m.import->dll));
    }
    std::vector<std::string> dll_order;
    std::map<std::string, std::vector<coff::ImportEntry>> by_dll;
    for (const auto& imp : imports) {
        if (covered.contains(to_lower(imp.dll))) continue;
        if (!by_dll.contains(imp.dll)) dll_order.push_back(imp.dll);
        coff::ImportEntry e;
        e.symbol = imp.symbol;
        e.name = imp.name;
        e.ordinal = imp.ordinal;
        e.hint = imp.hint;
        e.type = imp.thunk ? coff::import_type::code : coff::import_type::data;
        if (!coff::import_name_type_for(e, image.machine())) {
            result.notes.push_back(std::format("{}: {} cannot import {}; imported as {}", imp.dll, e.symbol, imp.name, relink::c_symbol(image, imp.name)));
            e.symbol = relink::c_symbol(image, imp.name);
        }
        by_dll[imp.dll].push_back(std::move(e));
    }
    for (const auto& dll : dll_order) {
        TRY_ASSIGN(auto bytes, coff::write_import_library(dll, image.machine(), by_dll[dll], import_library_id));
        const auto file = std::format("libs/{}.lib", file_part(fs::to_utf8(fs::from_utf8(dll).stem())));
        TRY(fs::write_file(dir / fs::from_utf8(file), bytes));
        result.libraries.push_back(file);
        inputs.push_back(fs::to_utf8(dir / fs::from_utf8(file)));
    }

    // Link.
    const auto linker = relink::linker_for(setup.toolchain, project.config().link.linker);
    relink::LinkRequest request;
    request.inputs = inputs;
    request.flags = relink::image_link_flags(image, entry);
    request.flags.insert(request.flags.end(), project.config().link.flags.begin(), project.config().link.flags.end());
    const auto name = program.path().filename();
    request.output = dir / "out" / name;
    if (const auto& cv = image.codeview(); cv && !cv->pdb_path.empty()) {
        const auto pdb_name = fs::from_utf8(replace_all(cv->pdb_path, "\\", "/")).filename();
        request.pdb = dir / "out" / (pdb_name.empty() ? std::filesystem::path(name).replace_extension(".pdb") : pdb_name);
    }
    request.work_dir = dir;
    request.cancelled = options.cancelled;
    progress(std::format("linking {} objects and libraries with {}", inputs.size(), linker.path));
    TRY_ASSIGN(result.link, relink::run_linker(linker, request));
    result.image = fs::to_utf8(std::filesystem::relative(request.output, project.root(), ec));
    if (!result.link.ok) {
        result.error = result.link.cancelled ? "the link was cancelled" : result.link.timed_out ? "the linker timed out" : "the link failed";
    } else {
        TRY_ASSIGN(auto bytes, fs::read_file(request.output));
        result.comparison = relink::compare_images(image, bytes, layout, program.symbols());
    }
    TRY(fs::write_text(dir / "result.json", to_json(result).dump(2) + "\n"));
    return result;
}

Json to_json(const RelinkResult& r) {
    Json units = Json::array();
    for (const auto& u : r.units) {
        Json j{{"unit", u.unit.name}, {"kind", std::string(to_string(u.unit.kind))}, {"mode", std::string(to_string(u.mode))},
               {"reason", u.reason}, {"bytes", u.bytes}};
        if (!u.object.empty()) j["object"] = u.object;
        if (u.check) j["check"] = to_json(*u.check);
        units.push_back(std::move(j));
    }
    Json link{{"ok", r.link.ok}, {"exit_code", r.link.exit_code}, {"output", r.link.output}, {"command", r.link.command},
              {"duration_ms", r.link.duration.count()}};
    Json out{{"time", r.time},
             {"identical", r.identical()},
             {"units", std::move(units)},
             {"libraries", r.libraries},
             {"notes", r.notes},
             {"link", std::move(link)},
             {"image", r.image}};
    if (r.comparison) out["comparison"] = relink::to_json(*r.comparison);
    if (!r.error.empty()) out["error"] = r.error;
    return out;
}

std::optional<Json> last_relink(const Project& project) {
    auto text = fs::read_text(relink_dir(project) / "result.json");
    if (!text) return std::nullopt;
    auto j = Json::parse(*text, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

} // namespace decomp::project
