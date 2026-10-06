#include "search/identify.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"

#include <algorithm>

namespace decomp::search {

namespace {

std::optional<Arch> arch_of_triple(std::string_view triple) {
    const std::string t = to_lower(triple);
    if (t.starts_with("x86_64") || t.starts_with("amd64") || t.starts_with("x64")) return Arch::x64;
    if (t.size() >= 4 && t[0] == 'i' && t[1] >= '3' && t[1] <= '6' && t.substr(2, 2) == "86") return Arch::x86;
    if (t.starts_with("x86")) return Arch::x86;
    return std::nullopt;
}

// A name that ends with or contains an architecture: "msvc-x86", "clang-cl-x64", "gcc_win64".
std::optional<Arch> arch_of_name(std::string_view name) {
    const std::string n = to_lower(name);
    for (std::string_view p : {"x86_64", "x86-64", "amd64", "x64", "win64"})
        if (n.find(p) != std::string::npos) return Arch::x64;
    for (std::string_view p : {"i386", "i486", "i586", "i686", "x86", "win32"})
        if (n.find(p) != std::string::npos) return Arch::x86;
    return std::nullopt;
}

} // namespace

std::optional<Arch> toolchain_arch(const matching::Toolchain& toolchain) {
    const auto& flags = toolchain.flags;
    for (usize i = 0; i < flags.size(); ++i) {
        const std::string_view f = flags[i];
        if (f.starts_with("--target=")) return arch_of_triple(f.substr(9));
        if (f.starts_with("-target=")) return arch_of_triple(f.substr(8));
        if ((f == "-target" || f == "--target") && i + 1 < flags.size()) return arch_of_triple(flags[i + 1]);
        if (f == "-m32") return Arch::x86;
        if (f == "-m64") return Arch::x64;
    }
    const auto compiler = fs::from_utf8(toolchain.compiler);
    if (toolchain.kind == matching::ToolchainKind::msvc) {
        // ...\bin\Hostx64\x86\cl.exe, ...\VC\bin\amd64\cl.exe: the directory names the target.
        const std::string dir = to_lower(fs::to_utf8(compiler.parent_path().filename()));
        if (dir == "x86") return Arch::x86;
        if (dir == "x64" || dir == "amd64" || dir.ends_with("_amd64") || dir.ends_with("_x64")) return Arch::x64;
    } else {
        // i686-w64-mingw32-gcc, x86_64-w64-mingw32-clang
        const std::string file = fs::to_utf8(compiler.filename());
        if (file.find('-') != std::string::npos)
            if (auto a = arch_of_triple(file)) return a;
    }
    return arch_of_name(toolchain.name);
}

bool IdentifyResult::decided() const {
    if (ranking.empty()) return false;
    const Score& top = ranking.front().score;
    if (top.exact == 0 && top.match_percent <= 0) return false;  // nothing compiled
    return ranking.size() == 1 || top.better_than(ranking[1].score);
}

IdentifyResult identify(const Program& program, const matching::MatchSetup& setup, std::span<const Probe> probes,
                        std::span<const matching::Toolchain> toolchains, const IdentifyOptions& options) {
    IdentifyResult r;
    auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    const bool start_msvc_style = setup.toolchain.msvc_style();
    for (const auto& toolchain : toolchains) {
        if (cancelled()) {
            r.cancelled = true;
            break;
        }
        ToolchainRank rank;
        rank.toolchain = toolchain.name;
        rank.kind = toolchain.kind;
        matching::MatchSetup s = setup;
        s.toolchain = toolchain;
        s.cancelled = options.cancelled;
        auto groups = preset_groups(options.preset, toolchain.kind, program.arch());
        if (!groups) {
            rank.error = groups.error().message;
            r.ranking.push_back(std::move(rank));
            continue;
        }
        // The flags given are kept for a toolchain that reads them (the same style); others start bare.
        const std::vector<std::string> start = toolchain.msvc_style() == start_msvc_style ? options.start : std::vector<std::string>{};
        // A toolchain that cannot compile the probes at all (a missing compiler, a language it does not
        // take) is not searched.
        const Configuration first{toolchain, flags_of(base_flags(start, *groups, toolchain.msvc_style()), *groups,
                                                      choice_of(start, *groups, toolchain.msvc_style()))};
        const auto probe = evaluate(program, s, first, probes);
        if (probe.cancelled) {
            r.cancelled = true;
            break;
        }
        if (!probe.compile_error.empty() && std::ranges::none_of(probe.functions, &FunctionScore::found)) {
            ++r.candidates;
            rank.error = probe.compile_error;
            rank.flags = first.flags;
            rank.score = probe.score;
            rank.candidates = 1;
            if (options.log) options.log->add(toolchain.name + ": " + choice_label(*groups, choice_of(start, *groups, toolchain.msvc_style())), probe.score);
            r.ranking.push_back(std::move(rank));
            continue;
        }
        FlagSearchOptions fo;
        fo.groups = *groups;
        fo.start = start;
        fo.exhaustive_limit = options.max_per_toolchain;
        fo.max_candidates = options.max_per_toolchain;
        fo.threads = options.threads;
        fo.cancelled = options.cancelled;
        CandidateLog labeled(nullptr, [&](const LogEntry& e) {
            if (options.log) options.log->add(toolchain.name + ": " + e.label, e.score);
        });
        fo.log = &labeled;
        const auto found = search_flags(program, s, probes, fo);
        rank.flags = found.flags;
        rank.score = found.score;
        rank.candidates = found.candidates;
        r.candidates += found.candidates;
        r.ranking.push_back(std::move(rank));
        if (found.cancelled) {
            r.cancelled = true;
            break;
        }
    }
    std::ranges::stable_sort(r.ranking, [](const ToolchainRank& a, const ToolchainRank& b) { return a.score.better_than(b.score); });
    return r;
}

Json to_json(const IdentifyResult& result) {
    Json ranking = Json::array();
    for (const auto& t : result.ranking) {
        Json j{{"toolchain", t.toolchain},
               {"kind", std::string(matching::to_string(t.kind))},
               {"flags", t.flags},
               {"score", to_json(t.score)},
               {"candidates", t.candidates}};
        if (!t.error.empty()) j["error"] = t.error;
        ranking.push_back(std::move(j));
    }
    return {{"ranking", std::move(ranking)}, {"candidates", result.candidates}, {"cancelled", result.cancelled}, {"decided", result.decided()}};
}

} // namespace decomp::search
