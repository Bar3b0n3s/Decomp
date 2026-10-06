#include "search/flags.hpp"

#include "core/strings.hpp"
#include "matching/toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <random>

namespace decomp::search {

namespace {

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    usize i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const usize start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

// MSVC-style compilers take "-O2" for "/O2".
std::string normalized(std::string_view flag, bool msvc_style) {
    std::string s(flag);
    if (msvc_style && !s.empty() && s[0] == '-') s[0] = '/';
    return s;
}

struct PresetGroup {
    int presets;  // kBasic | kCommon | kFull: the presets it is in
    bool msvc_style;
    int arch;     // 0: any, 32: x86, 64: x64
    std::string_view text;
};

constexpr int kBasic = 1, kCommon = 2, kFull = 4;

// The first alternative listed of equivalent ones wins a tie, so the defaults come first.
constexpr PresetGroup kPresetGroups[] = {
    // cl.exe and clang-cl
    {kBasic, true, 0, "optimization: /Od | /O1 | /O2"},
    {kCommon | kFull, true, 0, "optimization: /Od | /O1 | /O2 | /Ox"},
    {kBasic | kCommon | kFull, true, 32, "frame pointers: none | /Oy-"},
    {kCommon | kFull, true, 0, "inlining: none | /Ob0 | /Ob1 | /Ob2"},
    {kBasic | kCommon | kFull, true, 0, "security checks: none | /GS-"},
    {kCommon | kFull, true, 0, "floating point: none | /fp:fast | /fp:strict"},
    {kCommon | kFull, true, 32, "instruction set: none | /arch:IA32 | /arch:SSE | /arch:SSE2 | /arch:AVX | /arch:AVX2"},
    {kCommon | kFull, true, 64, "instruction set: none | /arch:AVX | /arch:AVX2"},
    {kFull, true, 0, "intrinsics: none | /Oi | /Oi-"},
    {kFull, true, 0, "size or speed: none | /Os | /Ot"},
    {kFull, true, 0, "exceptions: none | /EHsc | /EHs | /EHa"},
    {kFull, true, 0, "char: none | /J"},
    {kFull, true, 0, "packing: none | /Zp1 | /Zp2 | /Zp4 | /Zp16"},
    {kFull, true, 0, "hot patching: none | /hotpatch"},
    {kFull, true, 64, "tuning: none | /favor:INTEL64 | /favor:AMD64"},
    // gcc and clang
    {kBasic, false, 0, "optimization: -O0 | -O1 | -O2 | -O3 | -Os"},
    {kCommon | kFull, false, 0, "optimization: -O0 | -Og | -O1 | -O2 | -O3 | -Os"},
    {kBasic, false, 0, "frame pointers: none | -fno-omit-frame-pointer"},
    {kCommon | kFull, false, 0, "frame pointers: none | -fomit-frame-pointer | -fno-omit-frame-pointer"},
    {kCommon | kFull, false, 0, "inlining: none | -fno-inline | -fno-inline-small-functions | -finline-functions"},
    {kCommon | kFull, false, 32, "instruction set: none | -march=i386 | -march=i686 | -march=pentium4 | -march=core2"},
    {kCommon | kFull, false, 64, "instruction set: none | -march=x86-64-v2 | -march=x86-64-v3"},
    {kFull, false, 32, "x87 or SSE: none | -mfpmath=387 | -mfpmath=sse"},
    {kFull, false, 0, "aliasing: none | -fno-strict-aliasing"},
    {kFull, false, 0, "loops: none | -funroll-loops"},
    {kFull, false, 0, "floating point: none | -ffast-math"},
    {kFull, false, 0, "vectorization: none | -fno-tree-vectorize"},
    {kFull, false, 0, "char: none | -funsigned-char"},
};

using Choice = std::vector<usize>;

std::vector<Choice> neighbors_of(const Choice& c, std::span<const FlagGroup> groups) {
    std::vector<Choice> out;
    for (usize g = 0; g < groups.size(); ++g)
        for (usize a = 0; a < groups[g].alternatives.size(); ++a)
            if (a != c[g]) {
                Choice n = c;
                n[g] = a;
                out.push_back(std::move(n));
            }
    return out;
}

usize differences(const Choice& a, const Choice& b) {
    usize n = 0;
    for (usize i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

// Evaluates configurations once each (a choice per group), within the budget, and remembers the scores.
class Engine {
public:
    Engine(const Program& program, const matching::MatchSetup& setup, std::span<const Probe> probes, const FlagSearchOptions& options,
           std::span<const std::string> base)
        : program_(program), setup_(setup), probes_(probes), options_(options), base_(base) {
        setup_.cancelled = options.cancelled;
    }

    bool cancelled() const { return cancelled_ || (options_.cancelled && options_.cancelled()); }
    bool stopped() const { return cancelled() || evaluated_ >= options_.max_candidates; }
    usize evaluated() const { return evaluated_; }
    const Score* known(const Choice& c) const {
        auto it = memo_.find(c);
        return it == memo_.end() ? nullptr : &it->second;
    }

    // Compiles the choices not evaluated yet, in parallel, as many as the budget allows.
    void evaluate(std::vector<Choice> choices) {
        std::vector<Choice> todo;
        for (auto& c : choices)
            if (!memo_.contains(c) && std::ranges::find(todo, c) == todo.end()) todo.push_back(std::move(c));
        const usize room = options_.max_candidates - std::min(evaluated_, options_.max_candidates);
        if (todo.size() > room) todo.resize(room);
        std::vector<std::optional<Score>> scores(todo.size());
        const int threads = options_.threads > 0 ? options_.threads : matching::max_parallel_compiles();
        parallel_for(todo.size(), threads, [&](usize i) {
            const Configuration config{setup_.toolchain, flags_of(base_, options_.groups, todo[i])};
            auto e = search::evaluate(program_, setup_, config, probes_);
            if (!e.cancelled) scores[i] = e.score;
        }, [&] { return cancelled(); });
        for (usize i = 0; i < todo.size(); ++i) {  // logged in the order they were made
            if (!scores[i]) {
                cancelled_ = true;  // a job was skipped or its compile cancelled
                continue;
            }
            if (options_.log) options_.log->add(choice_label(options_.groups, todo[i]), *scores[i]);
            memo_.emplace(std::move(todo[i]), *scores[i]);
            ++evaluated_;
        }
    }

    // Moves from `current` to its best single-group change while that is better than staying.
    Choice descend(Choice current) {
        evaluate({current});
        while (known(current) && !stopped()) {
            auto neighbors = neighbors_of(current, options_.groups);
            evaluate(neighbors);
            const Score* best = known(current);
            const Choice* next = nullptr;
            for (const auto& n : neighbors)
                if (const Score* s = known(n); s && s->better_than(*best)) {
                    best = s;
                    next = &n;
                }
            if (!next) break;
            current = *next;
        }
        return current;
    }

private:
    const Program& program_;
    matching::MatchSetup setup_;
    std::span<const Probe> probes_;
    const FlagSearchOptions& options_;
    std::span<const std::string> base_;
    std::map<Choice, Score> memo_;
    usize evaluated_ = 0;
    bool cancelled_ = false;
};

} // namespace

Result<FlagGroup> parse_flag_group(std::string_view text) {
    FlagGroup g;
    std::string_view rest = trim(text);
    // An optional name: a letter, then letters, digits, spaces, '_' or '-', then a colon and a space.
    if (!rest.empty() && std::isalpha(static_cast<unsigned char>(rest[0]))) {
        usize i = 0;
        while (i < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[i])) || rest[i] == ' ' || rest[i] == '_' || rest[i] == '-')) ++i;
        if (i < rest.size() && rest[i] == ':' && (i + 1 == rest.size() || std::isspace(static_cast<unsigned char>(rest[i + 1])))) {
            g.name = std::string(trim(rest.substr(0, i)));
            rest = trim(rest.substr(i + 1));
        }
    }
    for (auto part : split(rest, '|')) {
        auto flags = words(part);
        if (flags.size() == 1 && flags[0] == "none") flags.clear();
        if (std::ranges::find(g.alternatives, flags) == g.alternatives.end()) g.alternatives.push_back(std::move(flags));
    }
    if (g.alternatives.size() < 2)
        return make_error(ErrorCode::invalid_argument, "a flag group needs two alternatives or more, separated by '|': '{}'", text);
    return g;
}

std::string alternative_text(std::span<const std::string> flags) {
    return flags.empty() ? "none" : join(std::vector<std::string>(flags.begin(), flags.end()), " ");
}

std::string to_string(const FlagGroup& group) {
    std::vector<std::string> parts;
    for (const auto& a : group.alternatives) parts.push_back(alternative_text(a));
    return (group.name.empty() ? "" : group.name + ": ") + join(parts, " | ");
}

std::vector<std::string> flag_presets() { return {"basic", "common", "full", "none"}; }

Result<std::vector<FlagGroup>> preset_groups(std::string_view preset, matching::ToolchainKind kind, Arch arch) {
    const int mask = preset == "basic" ? kBasic : preset == "common" ? kCommon : preset == "full" ? kFull : 0;
    if (mask == 0 && preset != "none") return make_error(ErrorCode::invalid_argument, "unknown flag preset '{}' (basic, common, full or none)", preset);
    std::vector<FlagGroup> out;
    const bool msvc_style = kind == matching::ToolchainKind::msvc || kind == matching::ToolchainKind::clang_cl;
    const int bits = arch == Arch::x86 ? 32 : 64;
    for (const auto& p : kPresetGroups) {
        if (p.msvc_style != msvc_style || (p.presets & mask) == 0 || (p.arch != 0 && p.arch != bits)) continue;
        TRY_ASSIGN(auto g, parse_flag_group(p.text));
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<std::string> base_flags(std::span<const std::string> flags, std::span<const FlagGroup> groups, bool msvc_style) {
    std::vector<std::string> grouped;
    for (const auto& g : groups)
        for (const auto& a : g.alternatives)
            for (const auto& f : a) grouped.push_back(normalized(f, msvc_style));
    std::vector<std::string> out;
    for (const auto& f : flags)
        if (std::ranges::find(grouped, normalized(f, msvc_style)) == grouped.end()) out.push_back(f);
    return out;
}

std::vector<usize> choice_of(std::span<const std::string> flags, std::span<const FlagGroup> groups, bool msvc_style) {
    std::vector<std::string> given;
    for (const auto& f : flags) given.push_back(normalized(f, msvc_style));
    std::vector<usize> out;
    for (const auto& g : groups) {
        constexpr usize none = std::numeric_limits<usize>::max();
        usize chosen = none, chosen_pos = 0, chosen_size = 0;
        for (usize a = 0; a < g.alternatives.size(); ++a) {
            const auto& alt = g.alternatives[a];
            if (alt.empty()) continue;
            // Where the alternative's last flag is, when all of them are there.
            usize pos = 0;
            bool present = true;
            for (const auto& f : alt) {
                auto it = std::ranges::find(given.rbegin(), given.rend(), normalized(f, msvc_style));
                if (it == given.rend()) {
                    present = false;
                    break;
                }
                pos = std::max(pos, static_cast<usize>(given.rend() - it));
            }
            if (present && (chosen == none || pos > chosen_pos || (pos == chosen_pos && alt.size() > chosen_size))) {
                chosen = a;
                chosen_pos = pos;
                chosen_size = alt.size();
            }
        }
        if (chosen == none) {
            auto empty = std::ranges::find_if(g.alternatives, [](const auto& alt) { return alt.empty(); });
            chosen = empty == g.alternatives.end() ? 0 : static_cast<usize>(empty - g.alternatives.begin());
        }
        out.push_back(chosen);
    }
    return out;
}

std::vector<std::string> flags_of(std::span<const std::string> base, std::span<const FlagGroup> groups, std::span<const usize> choice) {
    std::vector<std::string> out(base.begin(), base.end());
    for (usize g = 0; g < groups.size() && g < choice.size(); ++g) {
        const auto& alt = groups[g].alternatives[choice[g]];
        out.insert(out.end(), alt.begin(), alt.end());
    }
    return out;
}

std::string choice_label(std::span<const FlagGroup> groups, std::span<const usize> choice) {
    std::vector<std::string> parts;
    for (usize g = 0; g < groups.size() && g < choice.size(); ++g)
        if (const auto& alt = groups[g].alternatives[choice[g]]; !alt.empty()) parts.push_back(alternative_text(alt));
    return parts.empty() ? "defaults" : join(parts, " ");
}

FlagSearchResult search_flags(const Program& program, const matching::MatchSetup& setup, std::span<const Probe> probes,
                              const FlagSearchOptions& options) {
    FlagSearchResult r;
    const auto& groups = options.groups;
    const bool msvc_style = setup.toolchain.msvc_style();
    r.base = base_flags(options.start, groups, msvc_style);
    r.start_choice = choice_of(options.start, groups, msvc_style);
    r.space = 1;
    for (const auto& g : groups)
        r.space = r.space > std::numeric_limits<usize>::max() / g.alternatives.size() ? std::numeric_limits<usize>::max()
                                                                                       : r.space * g.alternatives.size();
    Engine engine(program, setup, probes, options, r.base);
    Choice best = r.start_choice;
    if (r.space <= options.exhaustive_limit && r.space <= options.max_candidates) {
        r.exhaustive = true;
        std::vector<Choice> all;
        Choice c(groups.size(), 0);
        for (;;) {
            all.push_back(c);
            usize g = 0;
            while (g < groups.size() && ++c[g] == groups[g].alternatives.size()) c[g++] = 0;
            if (g == groups.size()) break;
        }
        engine.evaluate({r.start_choice});  // logged first
        engine.evaluate(std::move(all));
        // The best; of equal ones, the one closest to the start, then the first.
        all.clear();
        Choice it(groups.size(), 0);
        for (;;) {
            if (const Score* s = engine.known(it)) {
                const Score* b = engine.known(best);
                if (!b || s->better_than(*b) ||
                    (*s == *b && differences(it, r.start_choice) < differences(best, r.start_choice)))
                    best = it;
            }
            usize g = 0;
            while (g < groups.size() && ++it[g] == groups[g].alternatives.size()) it[g++] = 0;
            if (g == groups.size()) break;
        }
    } else {
        best = engine.descend(r.start_choice);
        std::mt19937_64 rng(options.seed);
        for (int i = 0; i < options.restarts && !engine.stopped(); ++i) {
            if (const Score* s = engine.known(best); s && s->complete()) break;
            Choice start(groups.size());
            for (usize g = 0; g < groups.size(); ++g) start[g] = static_cast<usize>(rng() % groups[g].alternatives.size());
            Choice end = engine.descend(std::move(start));
            const Score* a = engine.known(end);
            const Score* b = engine.known(best);
            if (a && (!b || a->better_than(*b))) best = std::move(end);
        }
    }
    // Every single-group change of the best, for the alternatives that do as well.
    engine.evaluate(neighbors_of(best, groups));

    r.choice = best;
    r.flags = flags_of(r.base, groups, best);
    if (const Score* s = engine.known(best)) r.score = *s;
    if (const Score* s = engine.known(r.start_choice)) r.start_score = *s;
    for (usize g = 0; g < groups.size(); ++g) {
        std::vector<usize> same;
        for (usize a = 0; a < groups[g].alternatives.size(); ++a) {
            Choice c = best;
            c[g] = a;
            if (const Score* s = engine.known(c); s && *s == r.score) same.push_back(a);
        }
        r.equivalent.push_back(std::move(same));
    }
    r.candidates = engine.evaluated();
    r.cancelled = engine.cancelled();
    return r;
}

Json to_json(const FlagSearchResult& result, std::span<const FlagGroup> groups) {
    Json gs = Json::array();
    for (usize g = 0; g < groups.size(); ++g) {
        Json alternatives = Json::array();
        for (const auto& a : groups[g].alternatives) alternatives.push_back(alternative_text(a));
        Json j{{"name", groups[g].name}, {"alternatives", std::move(alternatives)}};
        if (g < result.start_choice.size()) j["start"] = result.start_choice[g];
        if (g < result.choice.size()) j["chosen"] = result.choice[g];
        if (g < result.equivalent.size()) j["equivalent"] = result.equivalent[g];
        gs.push_back(std::move(j));
    }
    return {{"flags", result.flags},
            {"base", result.base},
            {"groups", std::move(gs)},
            {"score", to_json(result.score)},
            {"start_score", to_json(result.start_score)},
            {"candidates", result.candidates},
            {"space", result.space},
            {"exhaustive", result.exhaustive},
            {"cancelled", result.cancelled}};
}

} // namespace decomp::search
