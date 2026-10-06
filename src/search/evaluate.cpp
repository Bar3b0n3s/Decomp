#include "search/evaluate.hpp"

#include "core/strings.hpp"
#include "matching/unit_source.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <thread>

namespace decomp::search {

std::string Configuration::label() const {
    std::string out = toolchain.name;
    for (const auto& f : flags) out += " " + f;
    return out;
}

u32 distance_of(const matching::FunctionDiff& diff) {
    if (diff.byte_exact) return 0;
    const u64 d = 10 * (u64(diff.inserted) + diff.deleted) + 6 * u64(diff.opcode) + 3 * u64(diff.operand) + u64(diff.encoding);
    return static_cast<u32>(std::clamp<u64>(d, 1, kMissingDistance - 1));
}

FunctionScore score_of(u64 va, const matching::FunctionDiff& diff) {
    FunctionScore s;
    s.va = va;
    s.found = true;
    s.byte_exact = diff.byte_exact;
    s.match_percent = diff.match_percent;
    s.distance = distance_of(diff);
    return s;
}

bool Score::better_than(const Score& other) const {
    if (exact != other.exact) return exact > other.exact;
    if (distance != other.distance) return distance < other.distance;
    return match_percent > other.match_percent + 1e-9;
}

std::string Score::text() const {
    return std::format("{}/{} byte-exact, distance {}, {:.1f}%", exact, functions, distance, match_percent);
}

Score total(std::span<const FunctionScore> functions) {
    Score s;
    s.functions = functions.size();
    double sum = 0;
    for (const auto& f : functions) {
        s.exact += f.byte_exact;
        s.distance += f.distance;
        sum += f.match_percent;
    }
    s.match_percent = functions.empty() ? 0 : sum / static_cast<double>(functions.size());
    return s;
}

Json to_json(const Score& score) {
    return {{"functions", score.functions}, {"exact", score.exact}, {"distance", score.distance}, {"match_percent", score.match_percent}};
}

Score score_from_json(const Json& j) {
    Score s;
    if (!j.is_object()) return s;
    s.functions = j.value("functions", usize{0});
    s.exact = j.value("exact", usize{0});
    s.distance = j.value("distance", u64{0});
    s.match_percent = j.value("match_percent", 0.0);
    return s;
}

Evaluation evaluate(const Program& program, const matching::MatchSetup& setup, const Configuration& configuration,
                    std::span<const Probe> probes) {
    const auto start = std::chrono::steady_clock::now();
    Evaluation out;
    matching::MatchSetup s = setup;
    s.toolchain = configuration.toolchain;
    s.flags = configuration.flags;
    for (const auto& probe : probes) {
        auto missing = [&](const std::string& why) {
            for (u64 va : probe.functions) {
                FunctionScore f;
                f.va = va;
                f.error = why;
                out.functions.push_back(std::move(f));
            }
        };
        auto v = matching::verify_unit(program, s, probe.source, probe.file_name, probe.functions);
        if (!v) {
            missing(v.error().message);
            continue;
        }
        if (v->compile.cancelled) out.cancelled = true;
        if (!v->error.empty()) {
            if (out.compile_error.empty())
                out.compile_error = v->compile.diagnostics.empty() ? std::string(trim(v->compile.output)) : matching::format_diagnostics(v->compile.diagnostics, 5);
            if (out.compile_error.empty()) out.compile_error = v->error;
            missing(v->error);
            continue;
        }
        for (const auto& check : v->functions) {
            if (check.diff) {
                out.functions.push_back(score_of(check.va, *check.diff));
            } else {
                FunctionScore f;
                f.va = check.va;
                f.error = check.error;
                out.functions.push_back(std::move(f));
            }
        }
    }
    out.score = total(out.functions);
    out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return out;
}

void parallel_for(usize count, int threads, const std::function<void(usize)>& job, const std::function<bool()>& cancelled) {
    if (count == 0) return;
    const usize workers = std::clamp<usize>(static_cast<usize>(std::max(threads, 1)), 1, count);
    std::atomic<usize> next{0};
    auto work = [&] {
        for (;;) {
            if (cancelled && cancelled()) return;
            const usize i = next.fetch_add(1);
            if (i >= count) return;
            job(i);
        }
    };
    if (workers == 1) {
        work();
        return;
    }
    std::vector<std::jthread> pool;
    pool.reserve(workers - 1);
    for (usize t = 1; t < workers; ++t) pool.emplace_back(work);
    work();
}

} // namespace decomp::search
