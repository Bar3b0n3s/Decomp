#pragma once

// The token-level source permuter (docs/matching.md#permuter): random but deterministic (seeded) edits
// of the bodies of a translation unit's target functions, kept while they bring the functions closer to
// the target's bytes. The edits keep the code equivalent where C allows it: independent statements and
// declarations reordered, declarators swapped or split, operands of commutative operators swapped,
// comparisons flipped, if/else branches swapped under a negated condition, ++i for i++. An edit can
// make code that is not equivalent (reordering statements that depend on each other); only a result
// whose functions are byte-exact is known to be equivalent, to the target itself.

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "matching/match.hpp"
#include "search/evaluate.hpp"
#include "search/runs.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::search {

enum class TokenKind : u8 { word, number, string, character, punct, directive };

// A token of C or C++ source. The whitespace and comments before it are its lead.
struct Token {
    TokenKind kind = TokenKind::punct;
    usize lead = 0;   // where its lead starts
    usize begin = 0;  // where it starts
    usize end = 0;
    std::string_view text(std::string_view source) const { return source.substr(begin, end - begin); }
};
// Words, numbers, string and character literals (with their prefixes, raw strings too), punctuators
// (the longest that fits) and preprocessor directives (a token each, to the end of the line).
std::vector<Token> tokenize(std::string_view source);

enum class MutationKind : u8 { move, swap_declarators, split_declaration, commute, flip_comparison, negate_if, increment };
std::string_view to_string(MutationKind kind);

struct Mutation {
    MutationKind kind = MutationKind::move;
    std::string description;  // "move `speed *= 0.5f;` down 2"
    std::string source;       // the whole source with the edit
};

// The bodies of the functions defined at the top level of `source` (or in its namespace and extern
// blocks) with one of `names`: their braces' token indices.
std::vector<std::pair<usize, usize>> function_bodies(std::string_view source, std::span<const Token> tokens, std::span<const std::string> names);

// Every single edit of the bodies of the functions with one of `names`, in source order.
std::vector<Mutation> all_mutations(std::string_view source, std::span<const std::string> names);
// One of them, at random (a kind first, then an edit of that kind); nullopt when none applies.
std::optional<Mutation> random_mutation(std::string_view source, std::span<const std::string> names, std::mt19937_64& rng);

struct PermuteOptions {
    usize max_candidates = 500;    // sources compiled, at most
    usize batch = 0;               // candidates per round (0: twice the threads, at least 8)
    int threads = 0;               // 0: as many as compiles may run at once
    u64 seed = 1;
    std::chrono::seconds time_limit{0};  // 0: none
    std::function<bool()> cancelled;
    CandidateLog* log = nullptr;   // every candidate evaluated
};

struct PermuteResult {
    std::string source;              // the best source
    std::vector<std::string> steps;  // the edits that made it from the start
    Score start_score, score;
    usize candidates = 0;            // sources compiled
    bool cancelled = false;
    std::string error;               // why nothing was searched
};

// Permutes `probe`'s source (the bodies of the functions named `names`), each candidate compiled with
// `configuration` and scored on the probe's functions. Rounds evaluate every single edit of the current
// source not evaluated yet (up to the batch) and stacks of two or three random ones, and move to the
// best when it is better (to an equal one now and then, to leave a plateau); the search ends when every
// function is byte-exact, at the candidate or time limit, or when no new candidate comes up.
PermuteResult permute(const Program& program, const matching::MatchSetup& setup, const Configuration& configuration, const Probe& probe,
                      std::span<const std::string> names, const PermuteOptions& options);

Json to_json(const PermuteResult& result);

} // namespace decomp::search
