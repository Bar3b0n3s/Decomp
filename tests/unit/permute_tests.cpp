// The token-level permuter (search/permute.hpp): tokens, function bodies, the edits it makes, and a
// search that turns reordered but equivalent sources back into byte-exact matches.

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "llvm_fixture.hpp"
#include "matching/unit_source.hpp"
#include "search/permute.hpp"
#include "search/probes.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>

using namespace decomp;
using namespace decomp::search;

namespace {

std::vector<std::string> texts(std::string_view source, const std::vector<Token>& tokens) {
    std::vector<std::string> out;
    for (const auto& t : tokens) out.emplace_back(t.text(source));
    return out;
}

const Mutation* find(const std::vector<Mutation>& ms, MutationKind kind, std::string_view source) {
    auto it = std::ranges::find_if(ms, [&](const Mutation& m) { return m.kind == kind && m.source == source; });
    return it == ms.end() ? nullptr : &*it;
}

std::vector<std::string> names(std::initializer_list<const char*> list) { return {list.begin(), list.end()}; }

} // namespace

TEST_CASE("permuter: tokens") {
    const std::string_view s = "#include <x.h>\n"
                               "int f(int a) { // note\n"
                               "  return a+=1.5e+3f, 0x1e+2, L\"s\\\"\" R\"x(a)\" b)x\" 'c' >>= ::g->h; /* c */\n"
                               "}\n";
    const auto tokens = tokenize(s);
    CHECK(texts(s, tokens) == std::vector<std::string>{"#include <x.h>", "int", "f", "(", "int", "a", ")", "{", "return", "a", "+=", "1.5e+3f",
                                                       ",", "0x1e", "+", "2", ",", "L\"s\\\"\"", "R\"x(a)\" b)x\"", "'c'", ">>=", "::", "g",
                                                       "->", "h", ";", "}"});
    CHECK(tokens[0].kind == TokenKind::directive);
    CHECK(tokens[11].kind == TokenKind::number);
    CHECK(tokens[17].kind == TokenKind::string);
    CHECK(tokens[18].kind == TokenKind::string);
    CHECK(tokens[19].kind == TokenKind::character);
    // The lead of a token: what is between it and the token before, comments too.
    CHECK(s.substr(tokens[8].lead, tokens[8].begin - tokens[8].lead) == " // note\n  ");
    CHECK(s.substr(tokens.back().lead, tokens.back().begin - tokens.back().lead) == " /* c */\n");
}

TEST_CASE("permuter: the bodies of the functions named") {
    const std::string s = "struct P { int hp; void Hit(int); };\n"
                          "void P::Hit(int d) { hp -= d; }\n"
                          "namespace n { int g(int x) { return x; } }\n"
                          "extern \"C\" { int h() { return 1; } }\n"
                          "int other() { return 2; }\n";
    const auto tokens = tokenize(s);
    auto bodies = function_bodies(s, tokens, names({"P::Hit", "n::g", "h"}));
    REQUIRE(bodies.size() == 3);
    CHECK(tokens[bodies[0].first].text(s) == "{");
    CHECK(s.substr(tokens[bodies[0].first].begin, tokens[bodies[0].second].end - tokens[bodies[0].first].begin) == "{ hp -= d; }");
    CHECK(s.substr(tokens[bodies[1].first].begin, tokens[bodies[1].second].end - tokens[bodies[1].first].begin) == "{ return x; }");
    CHECK(s.substr(tokens[bodies[2].first].begin, tokens[bodies[2].second].end - tokens[bodies[2].first].begin) == "{ return 1; }");
    CHECK(function_bodies(s, tokens, names({"missing"})).empty());
}

TEST_CASE("permuter: statements move within their block, not past what they depend on") {
    const std::string s = "void f(int x) {\n"
                          "    a = x;\n"
                          "    int t = x * 2;\n"
                          "    b = t;\n"
                          "    if (x) return;\n"
                          "    c = 1;\n"
                          "}\n";
    const auto ms = all_mutations(s, names({"f"}));
    // a = x; moves down past the declaration of t (it does not use t), not past b = t; and the return.
    CHECK(find(ms, MutationKind::move, "void f(int x) {\n    int t = x * 2;\n    a = x;\n    b = t;\n    if (x) return;\n    c = 1;\n}\n"));
    CHECK(find(ms, MutationKind::move, "void f(int x) {\n    int t = x * 2;\n    b = t;\n    a = x;\n    if (x) return;\n    c = 1;\n}\n"));
    // The declaration of t does not move below b = t, which uses it.
    CHECK_FALSE(find(ms, MutationKind::move, "void f(int x) {\n    a = x;\n    b = t;\n    int t = x * 2;\n    if (x) return;\n    c = 1;\n}\n"));
    // Nothing moves across the return: the `if` holding it moves (the return is inside it).
    CHECK(find(ms, MutationKind::move, "void f(int x) {\n    a = x;\n    int t = x * 2;\n    b = t;\n    c = 1;\n    if (x) return;\n}\n"));
    for (const auto& m : ms)
        if (m.kind == MutationKind::move) CHECK(m.description.starts_with("move `"));

    const std::string labels = "int g(int v) { switch (v) { case 1: a = 1; b = 2; break; default: c = 3; } return 0; }\n";
    for (const auto& m : all_mutations(labels, names({"g"})))
        if (m.kind == MutationKind::move) CHECK(m.source == "int g(int v) { switch (v) { case 1: b = 2; a = 1; break; default: c = 3; } return 0; }\n");
}

TEST_CASE("permuter: declarators, operands, comparisons, branches and increments") {
    SUBCASE("declarators swapped and split") {
        const std::string s = "void f() {\n  int a = 1, *b, c[4];\n  use(a, b, c);\n}\n";
        const auto ms = all_mutations(s, names({"f"}));
        CHECK(find(ms, MutationKind::swap_declarators, "void f() {\n  int *b, a = 1, c[4];\n  use(a, b, c);\n}\n"));
        CHECK(find(ms, MutationKind::swap_declarators, "void f() {\n  int a = 1, c[4], *b;\n  use(a, b, c);\n}\n"));
        CHECK(find(ms, MutationKind::split_declaration, "void f() {\n  int a = 1;\n  int *b;\n  int c[4];\n  use(a, b, c);\n}\n"));
    }
    SUBCASE("operands of whole operands only") {
        const std::string s = "int f(int a, int b, int c) { return a + b * c + p->x[1]; }\n";
        const auto ms = all_mutations(s, names({"f"}));
        std::vector<std::string> commuted;
        for (const auto& m : ms)
            if (m.kind == MutationKind::commute) commuted.push_back(m.source);
        std::ranges::sort(commuted);
        // `a + b` (the left of the second +), `b * c`; never `b * c + p->x[1]` without `a`, nor `a + b` with `b` alone.
        CHECK(commuted == std::vector<std::string>{"int f(int a, int b, int c) { return a + c * b + p->x[1]; }\n",
                                                   "int f(int a, int b, int c) { return b * c + a + p->x[1]; }\n"});
    }
    SUBCASE("casts, calls and unary operators are operands") {
        const std::string s = "int f(int a) { return (int)g(a) & -a; }\n";
        const auto ms = all_mutations(s, names({"f"}));
        CHECK(find(ms, MutationKind::commute, "int f(int a) { return -a & (int)g(a); }\n"));
    }
    SUBCASE("&& and || only without side effects") {
        const auto ms = all_mutations("void f(int a, int b) { if (a > 0 && b) x(); if (g() || b) x(); }\n", names({"f"}));
        CHECK(find(ms, MutationKind::commute, "void f(int a, int b) { if (b && a > 0) x(); if (g() || b) x(); }\n"));
        CHECK_FALSE(find(ms, MutationKind::commute, "void f(int a, int b) { if (a > 0 && b) x(); if (b || g()) x(); }\n"));
        CHECK(find(ms, MutationKind::flip_comparison, "void f(int a, int b) { if (0 < a && b) x(); if (g() || b) x(); }\n"));
    }
    SUBCASE("if/else swapped under a negated condition") {
        const auto ms = all_mutations("void f(int a) { if (a < 0) x(); else { y(); } if (!(a)) x(); else if (a > 1) y(); else z(); }\n", names({"f"}));
        CHECK(find(ms, MutationKind::negate_if, "void f(int a) { if (!(a < 0)) { y(); } else x(); if (!(a)) x(); else if (a > 1) y(); else z(); }\n"));
        // !(a) becomes a; an else-if becomes the then-branch in braces.
        CHECK(find(ms, MutationKind::negate_if, "void f(int a) { if (a < 0) x(); else { y(); } if (a) { if (a > 1) y(); else z(); } else x(); }\n"));
    }
    SUBCASE("increments") {
        const auto ms = all_mutations("void f(int n) { for (int i = 0; i < n; i++) g(i); ++n; h(n++); }\n", names({"f"}));
        CHECK(find(ms, MutationKind::increment, "void f(int n) { for (int i = 0; i < n; ++i) g(i); ++n; h(n++); }\n"));
        CHECK(find(ms, MutationKind::increment, "void f(int n) { for (int i = 0; i < n; i++) g(i); n++; h(n++); }\n"));
        // A value that is used stays as it is.
        CHECK(std::ranges::count(ms, MutationKind::increment, &Mutation::kind) == 2);
    }
}

TEST_CASE("permuter: random edits are deterministic for a seed") {
    const std::string s = fs::read_text(test::fixture("src/permute_perturbed.cpp")).value();
    const auto targets = names({"Player::Hit", "two_buffers", "set_all", "weigh"});
    auto sequence = [&](u64 seed) {
        std::mt19937_64 rng(seed);
        std::vector<std::string> out;
        std::string text = s;
        for (int i = 0; i < 20; ++i) {
            auto m = random_mutation(text, targets, rng);
            REQUIRE(m);
            out.push_back(m->description);
            text = m->source;
        }
        return out;
    };
    CHECK(sequence(7) == sequence(7));
    CHECK(sequence(7) != sequence(8));
    // Only the functions named are edited.
    for (const auto& m : all_mutations(s, names({"weigh"}))) CHECK(m.source.find("speed *= 0.5f;\n    hp -= dmg;") != std::string::npos);
    CHECK_FALSE(random_mutation("int f() { return 1; }\n", names({"f"}), *std::make_unique<std::mt19937_64>(1)));
}

TEST_CASE("permuter: reordered statements and declarations back to byte-exact matches") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-permute").value();
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const auto dir = tmp.path() / std::string(to_string(arch));
        const auto exe = test::build_program(arch, *tools, dir, {test::fixture("src/permute.cpp")}, "permute");
        REQUIRE(exe);
        auto program = Program::open(*exe).value();
        auto probe = file_probe(program, test::fixture("src/permute_perturbed.cpp")).value();
        // Player::Hit, fill, two_buffers, set_all, weigh, entry
        CHECK(probe.functions.size() == 6);
        std::vector<std::string> targets;
        for (u64 va : probe.functions)
            for (auto& n : matching::definition_names(*program.symbols().at(va))) targets.push_back(std::move(n));
        const auto setup = test::clang_setup(arch, tools->clang_cl, dir / "work", dir / "cache");
        const Configuration configuration{setup.toolchain, setup.flags};

        PermuteOptions options;
        options.seed = 3;
        std::vector<LogEntry> entries;
        CandidateLog log(nullptr, [&](const LogEntry& e) { entries.push_back(e); });
        options.log = &log;
        const auto r = permute(program, setup, configuration, probe, targets, options);
        CHECK(r.error.empty());
        CHECK_FALSE(r.cancelled);
        // Four of the six functions are perturbed (three or four compile to other code); all are byte-exact after
        // the search.
        CHECK(r.start_score.exact <= 3);
        CHECK(r.score.complete());
        CHECK(r.score.exact == 6);
        CHECK(r.steps.size() >= 4);
        CHECK(r.candidates < options.max_candidates);
        CHECK(entries.front().label == "start");
        CHECK(log.count() == r.candidates);
        MESSAGE(to_string(arch) << ": " << r.candidates << " candidates; " << join(r.steps, "; "));
        // The best source is the perturbed one with its edits: it compiles to the target again.
        const std::vector<Probe> best = {Probe{r.source, probe.file_name, probe.functions}};
        CHECK(evaluate(program, setup, configuration, best).score.complete());
    }
}
