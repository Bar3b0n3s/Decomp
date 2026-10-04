#pragma once

// Helpers for the view-model tests: runs built from events with explicit times, and a project over the
// committed x86 fixture.

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "events/run_state.hpp"
#include "project/project.hpp"
#include "test_util.hpp"
#include "viewmodel/common.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace decomp::test {

// Applies events to a RunState at times the test chooses (seconds after t0).
struct EventScript {
    std::string run = "2026-10-04T10-00-00-0001";
    events::TimePoint t0 = vm::from_unix_ms(1'791'108'000'000);  // 2026-10-04 10:00:00 UTC
    events::TimePoint now = t0;
    events::RunState state;
    std::vector<events::Event> events;
    u64 seq = 0;

    void at(double seconds) { now = t0 + std::chrono::milliseconds(static_cast<long long>(seconds * 1000)); }
    const events::Event& add(events::Payload payload, int worker = -1) {
        events::Event e;
        e.seq = ++seq;
        e.time = now;
        e.run = run;
        e.worker = worker;
        e.payload = std::move(payload);
        state.apply(e);
        events.push_back(std::move(e));
        return events.back();
    }
    const events::RunStateData& data() const { return state.data(); }
    // A copy of the current state, as the GUI's snapshot would be.
    events::RunStateData snapshot() const { return state.data(); }
};

// A project over the committed x86 fixture (no compiler needed).
struct FixtureProject {
    fs::TempDir dir = fs::TempDir::create("decomp-vm").value();
    project::Project project = project::Project::init(dir.path() / "p", fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    Program program = project.open_program().value();

    u64 va(std::string_view name) const { return *program.resolve(name); }
    u32 size(std::string_view name) const { return program.symbols().at(va(name))->size; }
    void set(std::string_view name, project::FunctionStatus status, double best = 0, int attempts = 0, double cost = 0) {
        REQUIRE(project.update_function(va(name), project::FunctionInfo{status, best, attempts, cost}));
    }
};

} // namespace decomp::test
