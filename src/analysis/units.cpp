#include "analysis/units.hpp"

#include "analysis/program.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>

namespace decomp {

namespace {

std::string_view file_name(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view stem(std::string_view name) {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

bool is_source_file(std::string_view path) {
    for (std::string_view ext : {".c", ".cpp", ".cc", ".cxx", ".c++", ".cp"})
        if (iends_with(path, ext)) return true;
    return false;
}

// The directories of a path, outermost first ("C:\src\game\player.cpp" -> C:, src, game).
// A file or directory name that is safe in a project path on every host: characters other than letters,
// digits and "_-.+ " become '_', and a name of dots only becomes "_".
std::string path_component(std::string_view name) {
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '+' || c == ' ' ? c : '_';
    if (out.find_first_not_of('.') == std::string::npos) out = "_";
    return out;
}

// The directories of a path, outermost first, without its file, drive, `.` and `..` (a unit source never
// leaves src/).
std::vector<std::string> directories(std::string_view path) {
    std::vector<std::string> out;
    usize start = 0;
    for (usize i = 0; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/' && path[i] != '\\') continue;
        const std::string_view part = path.substr(start, i - start);
        if (i == path.size()) break;  // the file
        if (!part.empty() && part != "." && part != ".." && part.find(':') == std::string_view::npos) out.push_back(path_component(part));
        start = i + 1;
    }
    return out;
}

// The module's own source file among the files its line information names: the one with the object's
// stem, else the first C or C++ source.
std::string main_source(const pdb::Module& module) {
    const std::string object = lower(stem(file_name(module.name)));
    for (const auto& f : module.source_files)
        if (is_source_file(f) && lower(stem(file_name(f))) == object) return f;
    for (const auto& f : module.source_files)
        if (is_source_file(f)) return f;
    return {};
}

} // namespace

std::string_view to_string(UnitKind kind) {
    switch (kind) {
    case UnitKind::code: return "code";
    case UnitKind::library: return "library";
    case UnitKind::import: return "import";
    case UnitKind::linker: return "linker";
    }
    return "?";
}

std::optional<UnitKind> unit_kind_from_string(std::string_view text) {
    for (UnitKind k : {UnitKind::code, UnitKind::library, UnitKind::import, UnitKind::linker})
        if (to_string(k) == text) return k;
    return std::nullopt;
}

std::string_view to_string(UnitOrigin origin) {
    switch (origin) {
    case UnitOrigin::analysis: return "analysis";
    case UnitOrigin::map: return "map";
    case UnitOrigin::pdb: return "pdb";
    case UnitOrigin::user: return "user";
    }
    return "?";
}

std::optional<UnitOrigin> unit_origin_from_string(std::string_view text) {
    for (UnitOrigin o : {UnitOrigin::analysis, UnitOrigin::map, UnitOrigin::pdb, UnitOrigin::user})
        if (to_string(o) == text) return o;
    return std::nullopt;
}

const Unit* UnitLayout::find(std::string_view name) const {
    auto it = std::ranges::find(units, name, &Unit::name);
    return it == units.end() ? nullptr : &*it;
}

std::string_view UnitLayout::unit_of(u64 va) const {
    auto it = members.find(va);
    return it == members.end() ? std::string_view() : std::string_view(it->second);
}

std::vector<const Symbol*> UnitLayout::functions(std::string_view unit, const SymbolDb& symbols) const {
    std::vector<const Symbol*> out;
    for (const Symbol* f : symbols.functions())
        if (unit_of(f->va) == unit) out.push_back(f);
    return out;
}

std::string unit_name(const pdb::Module& module) {
    if (module.name == "* Linker *" || module.name.starts_with("Import:")) return module.name;
    const std::string_view library = file_name(module.object_name);
    if (!module.object_name.empty() && module.object_name != module.name && iends_with(library, ".lib"))
        return std::string(stem(library)) + ":" + std::string(file_name(module.name));
    return std::string(file_name(module.name));
}

std::string normalize_unit_name(std::string_view name) {
    const auto colon = name.find(':');
    if (colon == std::string_view::npos || name.starts_with("Import:")) return std::string(name);
    std::string_view library = name.substr(0, colon);
    if (iends_with(library, ".lib")) library.remove_suffix(4);
    return std::string(library) + std::string(name.substr(colon));
}

UnitKind unit_kind_of(std::string_view name) {
    if (name == "* Linker *" || name == "<linker-defined>") return UnitKind::linker;
    if (name.starts_with("Import:")) return UnitKind::import;
    if (const auto colon = name.find(':'); colon != std::string_view::npos)
        return iends_with(name.substr(colon + 1), ".dll") ? UnitKind::import : UnitKind::library;
    return UnitKind::code;
}

UnitLayout units_from_pdb(const pdb::Reader& pdb, const pe::Image& image, const SymbolDb& symbols) {
    UnitLayout layout;
    std::vector<std::string> names;
    std::map<std::string, int> seen;
    std::map<std::string, std::string> sources;
    for (const auto& module : pdb.modules()) {
        std::string name = unit_name(module);
        if (const int n = ++seen[name]; n > 1) name += std::format("#{}", n);
        names.push_back(name);
        layout.units.push_back(Unit{name, unit_kind_of(name), {}, UnitOrigin::pdb});
        if (layout.units.back().kind != UnitKind::code) continue;
        std::string source = main_source(module);
        // Without line information the compiler record still says C or C++.
        if (source.empty() && (module.language == 0 || module.language == 1))
            source = std::string(stem(file_name(module.name))) + (module.language == 0 ? ".c" : ".cpp");
        if (!source.empty()) sources[name] = std::move(source);
    }

    struct Range {
        u64 begin = 0, end = 0;
        u32 module = 0;
    };
    std::vector<Range> ranges;
    for (const auto& c : pdb.contributions())
        if (c.size && c.module < names.size()) ranges.push_back({image.image_base() + c.rva, image.image_base() + c.rva + c.size, c.module});
    std::ranges::sort(ranges, {}, &Range::begin);
    for (const auto& [va, s] : symbols) {
        if (s.kind == SymbolKind::label) continue;
        auto it = std::ranges::upper_bound(ranges, va, {}, &Range::begin);
        if (it == ranges.begin()) continue;
        --it;
        if (va < it->end) layout.members[va] = names[it->module];
    }
    assign_sources(layout, symbols, sources);
    return layout;
}

UnitLayout units_from_objects(const SymbolDb& symbols, const BinaryImage& image) {
    UnitLayout layout;
    struct First {
        u64 code = ~u64{0}, data = ~u64{0};
        bool map = false;
    };
    std::map<std::string, First> first;
    for (const auto& [va, s] : symbols) {
        if (s.object.empty() || s.kind == SymbolKind::label) continue;
        std::string name = normalize_unit_name(s.object);
        First& f = first[name];
        (image.is_code(va) ? f.code : f.data) = std::min(image.is_code(va) ? f.code : f.data, va);
        f.map = f.map || s.source == SymbolSource::map;
        layout.members[va] = std::move(name);
    }
    // A unit's code is one contiguous run of the code section: a function without an object file
    // belongs to the unit of the functions around it, or to the one before it.
    const auto functions = symbols.functions();
    std::string previous;
    for (usize i = 0; i < functions.size(); ++i) {
        const u64 va = functions[i]->va;
        if (!image.is_code(va)) continue;
        if (auto it = layout.members.find(va); it != layout.members.end()) {
            previous = it->second;
            continue;
        }
        if (!previous.empty()) layout.members[va] = previous;
    }
    std::vector<std::pair<std::string, First>> order(first.begin(), first.end());
    std::ranges::stable_sort(order, [](const auto& a, const auto& b) {
        const bool a_code = a.second.code != ~u64{0}, b_code = b.second.code != ~u64{0};
        if (a_code != b_code) return a_code;
        return (a_code ? a.second.code : a.second.data) < (b_code ? b.second.code : b.second.data);
    });
    for (const auto& [name, f] : order) layout.units.push_back(Unit{name, unit_kind_of(name), {}, f.map ? UnitOrigin::map : UnitOrigin::analysis});
    assign_sources(layout, symbols);
    return layout;
}

namespace {

// A source file name a function's string names (`__FILE__` in an assert or a log message): the unit it
// was compiled from. Only C and C++ sources count; a header names where an inline function came from.
std::string source_file_named(std::string_view text) {
    if (text.size() < 3 || text.size() > 260 || !is_source_file(text)) return {};
    const std::string_view file = file_name(text);
    if (file.size() < 3 || file.front() == '.') return {};
    for (char c : file)
        if (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == ' ')
            return {};
    return std::string(file);
}

} // namespace

UnitLayout units_by_analysis(const Program& program) {
    const SymbolDb& symbols = program.symbols();
    const pe::Image& image = program.image();
    UnitLayout layout;

    std::vector<const Symbol*> functions;
    for (const Symbol* f : symbols.functions())
        if (f->size && image.is_code(f->va)) functions.push_back(f);
    if (functions.empty()) return layout;
    const usize n = functions.size();

    // Units some record names (library matches, part of a map) and the linker's thunks.
    std::map<u64, std::string> import_dll;
    for (const auto& imp : image.imports()) import_dll[imp.iat_va] = imp.dll;
    std::vector<std::string> fixed(n);
    for (usize i = 0; i < n; ++i) {
        const Symbol& f = *functions[i];
        if (!f.object.empty()) {
            fixed[i] = normalize_unit_name(f.object);
        } else if (auto dest = program.thunk_destination(f.va)) {
            if (auto dll = import_dll.find(*dest); dll != import_dll.end())
                fixed[i] = lower(stem(dll->second)) + ":" + dll->second;
            else
                fixed[i] = "* Linker *";
        }
    }

    // What the other functions are tied by: calls between them, the data they use (data many functions
    // use is a global of the program and ties nothing), and source file names in their strings.
    std::map<u64, usize> index_of;
    for (usize i = 0; i < n; ++i) index_of[functions[i]->va] = i;
    std::vector<std::vector<usize>> calls(n);
    std::vector<std::vector<u64>> data(n);
    std::vector<std::string> named_file(n);
    std::map<u64, std::set<usize>> users;
    for (usize i = 0; i < n; ++i) {
        if (!fixed[i].empty()) continue;
        for (const Xref& x : program.xrefs_from(functions[i]->va)) {
            if (x.kind == XrefKind::call || x.kind == XrefKind::jump) {
                if (auto it = index_of.find(x.to); it != index_of.end() && it->second != i) calls[i].push_back(it->second);
            } else if (!image.is_code(x.to) && image.contains(x.to) && !import_dll.contains(x.to)) {
                data[i].push_back(x.to);
                users[x.to].insert(i);
                if (named_file[i].empty())
                    if (auto text = image.read_cstring(x.to, 300)) named_file[i] = source_file_named(*text);
            }
        }
    }
    constexpr usize kWindow = 12;      // functions apart that can still be tied
    constexpr usize kShared = 3;       // data more functions use is a global
    constexpr usize kNearbyUsers = 2;  // data placed together counts when this few functions use it
    constexpr u64 kNearby = 32;        // bytes apart
    constexpr usize kMinFunctions = 4; // a unit without a named source has at least this many functions
    auto tie = [&](usize i, usize j) {  // i < j
        double w = 0;
        for (usize c : calls[i]) w += c == j ? 1.0 : 0.0;
        for (usize c : calls[j]) w += c == i ? 1.0 : 0.0;
        for (u64 a : data[i])
            for (u64 b : data[j]) {
                if (image.section_at(a) != image.section_at(b)) continue;
                if (a == b) w += users[a].size() <= kShared ? 1.0 : 0.0;
                else if ((a > b ? a - b : b - a) <= kNearby && users[a].size() <= kNearbyUsers && users[b].size() <= kNearbyUsers) w += 0.5;
            }
        if (!named_file[i].empty() && named_file[i] == named_file[j]) w += 4.0;
        return w;
    };
    std::vector<double> crossing(n, 0.0);  // ties across the gap after each function
    for (usize i = 0; i < n; ++i)
        for (usize j = i + 1; j < n && j - i <= kWindow; ++j) {
            if (!fixed[i].empty() || !fixed[j].empty()) continue;
            if (const double w = tie(i, j); w > 0)
                for (usize g = i; g < j; ++g) crossing[g] += w;
        }
    // Functions naming the same source file belong to one unit, and so does everything between them.
    std::map<std::string, std::pair<usize, usize>> file_span;
    for (usize i = 0; i < n; ++i)
        if (!named_file[i].empty()) {
            auto [it, inserted] = file_span.emplace(lower(named_file[i]), std::pair{i, i});
            if (!inserted) it->second.second = i;
        }
    for (const auto& [file, span] : file_span)
        for (usize g = span.first; g < span.second; ++g) crossing[g] += 4.0;

    // Cut between consecutive functions where nothing ties the two sides and the unit so far has
    // enough functions, or where the named source file changes.
    std::string current, current_file;
    usize current_size = 0;
    bool current_fixed = false;
    std::vector<std::string> unit_of(n);
    for (usize i = 0; i < n; ++i) {
        std::string unit;
        const bool file_changes = !named_file[i].empty() && !current_file.empty() && lower(named_file[i]) != lower(current_file);
        if (!fixed[i].empty())
            unit = fixed[i];
        else if (current.empty() || current_fixed || file_changes || (crossing[i - 1] == 0 && current_size >= kMinFunctions))
            unit = {};  // a new unit, named below
        else
            unit = current;
        if (unit.empty()) {
            unit = std::format("unit_{:08x}.obj", functions[i]->va);
            current_file.clear();
            current_size = 0;
        } else if (unit != current) {
            current_file.clear();
            current_size = 0;
        }
        if (current_file.empty() && !named_file[i].empty()) current_file = named_file[i];
        ++current_size;
        current_fixed = !fixed[i].empty();
        unit_of[i] = unit;
        current = unit;
    }
    // A unit whose functions name a source file takes its name ("C:\\src\\player.cpp": player.obj, whose
    // source is src/player.cpp).
    std::map<std::string, std::string> file_of_unit;
    for (usize i = 0; i < n; ++i)
        if (fixed[i].empty() && !named_file[i].empty()) file_of_unit.emplace(unit_of[i], named_file[i]);
    std::set<std::string> used(unit_of.begin(), unit_of.end());
    std::map<std::string, std::string> rename, sources;
    for (const auto& [unit, file] : file_of_unit) {
        const std::string base(stem(file_name(file)));
        std::string name = base + ".obj";
        for (int k = 2; used.contains(name); ++k) name = std::format("{}_{}.obj", base, k);
        used.insert(name);
        rename[unit] = name;
        sources[name] = file;
    }
    std::set<std::string> listed;
    for (usize i = 0; i < n; ++i) {
        std::string unit = unit_of[i];
        if (auto it = rename.find(unit); it != rename.end()) unit = it->second;
        if (listed.insert(unit).second) layout.units.push_back(Unit{unit, unit_kind_of(unit), {}, UnitOrigin::analysis});
        layout.members[functions[i]->va] = std::move(unit);
    }
    // Data that one unit's functions alone use is that unit's.
    for (const auto& [d, who] : users) {
        std::set<std::string_view> units;
        for (usize i : who) units.insert(layout.unit_of(functions[i]->va));
        if (units.size() == 1 && symbols.at(d)) layout.members[d] = std::string(*units.begin());
    }
    assign_sources(layout, symbols, sources);
    return layout;
}

void assign_sources(UnitLayout& layout, const SymbolDb& symbols, const std::map<std::string, std::string>& sources, std::string_view dir) {
    struct Pending {
        Unit* unit = nullptr;
        std::string file;
        std::vector<std::string> dirs;  // of the original path, outermost first
        usize depth = 0;                // how many of them the path keeps
        std::string path() const {
            std::string out;
            for (usize k = dirs.size() - depth; k < dirs.size(); ++k) out += dirs[k] + "/";
            return out + file;
        }
    };
    std::set<std::string> taken;
    std::vector<Pending> pending;
    for (auto& u : layout.units) {
        if (u.kind != UnitKind::code) continue;
        if (!u.source.empty()) {
            taken.insert(lower(u.source));
            continue;
        }
        Pending p;
        p.unit = &u;
        if (auto it = sources.find(u.name); it != sources.end()) {
            p.file = path_component(file_name(it->second));
            p.dirs = directories(it->second);
        } else {
            bool cpp = false, any = false;
            for (const Symbol* f : layout.functions(u.name, symbols)) {
                any = true;
                cpp = cpp || f->name.starts_with('?');
            }
            p.file = path_component(stem(file_name(u.name))) + (any && !cpp ? ".c" : ".cpp");
        }
        pending.push_back(std::move(p));
    }
    // Add directories to the paths that collide until they differ.
    for (bool changed = true; changed;) {
        changed = false;
        std::map<std::string, std::vector<Pending*>> by_path;
        for (auto& p : pending) by_path[lower(p.path())].push_back(&p);
        for (auto& [path, group] : by_path) {
            if (group.size() < 2) continue;
            for (Pending* p : group)
                if (p->depth < p->dirs.size()) {
                    ++p->depth;
                    changed = true;
                }
        }
    }
    for (auto& p : pending) {
        std::string path = std::string(dir) + "/" + p.path();
        for (int n = 2; taken.contains(lower(path)); ++n)
            path = std::string(dir) + "/" + std::string(stem(p.path())) + std::format("_{}", n) +
                   std::string(p.file.substr(stem(p.file).size()));
        taken.insert(lower(path));
        p.unit->source = std::move(path);
    }
}

UnitComparison compare_units(const UnitLayout& truth, const UnitLayout& found, const SymbolDb& symbols) {
    UnitComparison c;
    std::vector<u64> functions;
    for (const Symbol* f : symbols.functions())
        if (!truth.unit_of(f->va).empty()) functions.push_back(f->va);
    c.functions = functions.size();
    std::map<std::string_view, std::vector<u64>> truth_units, found_units;
    for (u64 va : functions) {
        truth_units[truth.unit_of(va)].push_back(va);
        found_units[found.unit_of(va)].push_back(va);
    }
    c.truth_units = truth_units.size();
    c.found_units = found_units.size() - (found_units.contains(std::string_view()) ? 1 : 0);
    for (const auto& [name, members] : truth_units) {
        const std::string_view f = found.unit_of(members.front());
        if (!f.empty() && found_units[f] == members) {
            ++c.exact_units;
            c.functions_in_exact_units += members.size();
        }
    }
    c.same_names = true;
    for (usize k = 0; k < functions.size(); ++k) {
        const std::string_view t = truth.unit_of(functions[k]), f = found.unit_of(functions[k]);
        if (t != f) {
            c.same_names = false;
            if (c.differences.size() < 10) {
                const Symbol* s = symbols.at(functions[k]);
                c.differences.push_back(std::format("{:#x} {}: {} in the truth, {}", functions[k], s ? s->name : std::string(), t,
                                                    f.empty() ? std::string("in no unit") : std::format("{} found", f)));
            }
        }
        if (k + 1 == functions.size()) break;
        const bool tb = truth.unit_of(functions[k + 1]) != t;
        const bool fb = found.unit_of(functions[k + 1]) != f;
        c.truth_boundaries += tb;
        c.found_boundaries += fb;
        c.common_boundaries += tb && fb;
    }
    return c;
}

Json to_json(const UnitComparison& c) {
    return Json{{"truth_units", c.truth_units},
                {"found_units", c.found_units},
                {"functions", c.functions},
                {"exact_units", c.exact_units},
                {"functions_in_exact_units", c.functions_in_exact_units},
                {"exact_rate", c.exact_rate()},
                {"truth_boundaries", c.truth_boundaries},
                {"found_boundaries", c.found_boundaries},
                {"common_boundaries", c.common_boundaries},
                {"boundary_precision", c.boundary_precision()},
                {"boundary_recall", c.boundary_recall()},
                {"same_names", c.same_names},
                {"differences", c.differences}};
}

} // namespace decomp
