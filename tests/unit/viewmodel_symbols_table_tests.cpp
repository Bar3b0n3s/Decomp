#include "viewmodel/symbols_table.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::vm;
using project::ChangeOrigin;
using project::SymbolEdit;

namespace {

SymbolDb symbols_of(const project::Project& p) {
    SymbolDb db;
    for (const Symbol& s : p.symbols()) db.add(s);
    return db;
}

std::function<std::optional<SymbolState>(u64)> current_of(const project::Project& p) {
    auto states = std::make_shared<std::map<u64, SymbolState>>();
    for (const Symbol& s : p.symbols()) states->emplace(s.va, symbol_state(s));
    return [states](u64 va) -> std::optional<SymbolState> {
        auto it = states->find(va);
        return it == states->end() ? std::nullopt : std::optional(it->second);
    };
}

// Applies a revert plan the way the Symbols view does: as user changes.
void apply_plan(project::Project& p, const std::vector<RevertStep>& plan) {
    for (const auto& step : plan) REQUIRE(p.set_symbol(step.edit, ChangeOrigin{SymbolSource::user, "", "revert"}));
}

} // namespace

TEST_CASE("symbols: the log read back, rows and edit descriptions") {
    test::FixtureProject fx;
    const u64 add = fx.va("add");
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = add, .name = std::string("?add2@@YAHHH@Z")}, ChangeOrigin{SymbolSource::agent, "s-1", "binding"}));
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = 0x403030, .name = std::string("g_new"), .kind = SymbolKind::data, .size = 4u},
                                  ChangeOrigin{SymbolSource::agent, "s-1", "new global"}));
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = fx.va("read_counter"), .name = std::string("?rc@@YAHXZ")}, ChangeOrigin{SymbolSource::user, "", ""}));
    std::vector<Json> lines = fx.project.symbol_log();
    lines.insert(lines.begin() + 1, Json("garbage"));
    const auto log = parse_symbol_log(lines);
    REQUIRE(log.size() == 3);
    CHECK(log[0].index == 0);
    CHECK(log[1].index == 2);  // the malformed line keeps its place
    CHECK(log[0].va == add);
    CHECK(log[0].by_agent());
    CHECK(log[0].session == "s-1");
    CHECK(log[0].reason == "binding");
    REQUIRE(log[0].before);
    CHECK(log[0].before->name == "?add@@YAHHH@Z");
    CHECK(log[0].before->source == SymbolSource::pdb_public);
    CHECK(log[0].after->name == "?add2@@YAHHH@Z");
    CHECK(log[0].after->source == SymbolSource::agent);
    CHECK(log[0].time != TimePoint{});
    CHECK_FALSE(log[1].before);
    CHECK_FALSE(log[2].by_agent());

    CHECK(describe_edit(log[0].before, log[0].after) == "renamed ?add@@YAHHH@Z -> ?add2@@YAHHH@Z");
    CHECK(describe_edit(log[1].before, log[1].after) == "created data g_new (4 bytes)");
    CHECK(describe_edit(log[1].after, std::nullopt) == "removed g_new");
    SymbolState sized = *log[1].after;
    sized.size = 8;
    sized.kind = SymbolKind::string;
    CHECK(describe_edit(log[1].after, sized) == "kind data -> string; size 4 -> 8");

    const SymbolDb db = symbols_of(fx.project);
    fx.set("sum_array", project::FunctionStatus::nonmatching, 40);
    const auto rows = build_symbol_rows(db, *fx.project.function_infos(), &log);
    REQUIRE(rows.size() == db.size());
    CHECK(std::ranges::is_sorted(rows, {}, &SymbolRow::va));
    const auto& add_row = *std::ranges::find(rows, add, &SymbolRow::va);
    CHECK(add_row.name == "?add2@@YAHHH@Z");
    CHECK(add_row.edits == 1);
    CHECK(add_row.agent_edited);
    CHECK(add_row.status == project::FunctionStatus::unstarted);
    CHECK(std::ranges::find(rows, fx.va("sum_array"), &SymbolRow::va)->status == project::FunctionStatus::nonmatching);
    const auto& counter = *std::ranges::find(rows, fx.va("g_counter"), &SymbolRow::va);
    CHECK_FALSE(counter.status);  // data has no status
    CHECK(counter.edits == 0);
    CHECK(std::ranges::find(rows, u64{0x403030}, &SymbolRow::va)->kind == SymbolKind::data);
}

TEST_CASE("symbols: filter and sort") {
    test::FixtureProject fx;
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = fx.va("mix"), .name = std::string("?mix2@@YANNN@Z")}, ChangeOrigin{SymbolSource::agent, "s", ""}));
    const auto log = parse_symbol_log(fx.project.symbol_log());
    const auto rows = build_symbol_rows(symbols_of(fx.project), *fx.project.function_infos(), &log);
    auto run = [&](const SymbolFilter& f, std::vector<SymbolSortKey> sort = {}) { return filter_and_sort_symbols(rows, f, sort).value(); };
    CHECK(run({}).size() == rows.size());
    SymbolFilter text;
    text.text = "PLAYER::";
    CHECK(run(text).size() == 2);  // the demangled names, ignoring case
    text.text = "0x401065";  // an address inside add
    REQUIRE(run(text).size() == 1);
    CHECK(rows[run(text)[0]].va == fx.va("add"));
    text.text = "401060h";
    CHECK(run(text).size() == 1);
    SymbolFilter kinds;
    kinds.kinds = {SymbolKind::float_const, SymbolKind::string};
    CHECK(run(kinds).size() == 5);
    SymbolFilter sources;
    sources.sources = {SymbolSource::agent};
    CHECK(run(sources).size() == 1);
    SymbolFilter edited;
    edited.edited_only = true;
    CHECK(rows[run(edited).at(0)].va == fx.va("mix"));

    const auto by_size = run({}, {{SymbolColumn::size, true}});
    for (usize i = 1; i < by_size.size(); ++i) CHECK(rows[by_size[i - 1]].size >= rows[by_size[i]].size);
    const auto by_status = run({}, {{SymbolColumn::status, true}});
    CHECK_FALSE(rows[by_status.back()].status);  // symbols without a status last in both directions
    const auto by_kind = run({}, {{SymbolColumn::kind, false}, {SymbolColumn::display, false}});
    CHECK(rows[by_kind.front()].kind == SymbolKind::function);
    CHECK(to_string(SymbolColumn::edits) == "edits");
    CHECK(symbol_column_from_string("display") == SymbolColumn::display);
    CHECK_FALSE(symbol_column_from_string("x"));
    int polls = 0;
    CHECK_FALSE(filter_and_sort_symbols(rows, {}, {}, [&] { return ++polls > 0; }));
}

TEST_CASE("symbols: provenance from the log and the shown run, agent edits by session") {
    test::FixtureProject fx;
    const u64 add = fx.va("add");
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = add, .name = std::string("?add2@@YAHHH@Z")}, ChangeOrigin{SymbolSource::agent, "s-1", ""}));
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = fx.va("helper"), .name = std::string("helper2")}, ChangeOrigin{SymbolSource::agent, "s-2", ""}));
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = add, .size = 16u}, ChangeOrigin{SymbolSource::user, "", "fix size"}));
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = fx.va("mix"), .name = std::string("?mix2@@YANNN@Z")}, ChangeOrigin{SymbolSource::agent, "s-1", ""}));
    const auto log = parse_symbol_log(fx.project.symbol_log());

    test::EventScript run;
    run.add(events::SymbolChanged{add, "?add2@@YAHHH@Z", "?add2@@YAHHH@Z", "function", 15, "agent", "s-1"});  // logged already
    run.add(events::SymbolChanged{add, "?add2@@YAHHH@Z", "?add3@@YAHHH@Z", "function", 16, "agent", "s-9"});
    run.add(events::SymbolChanged{fx.va("mix"), "a", "b", "function", 0, "agent", "s-9"});
    const auto entries = symbol_provenance(add, log, &run.data());
    REQUIRE(entries.size() == 3);
    CHECK(std::ranges::is_sorted(entries, {}, &ProvenanceEntry::time));
    CHECK(std::ranges::count_if(entries, [](const ProvenanceEntry& e) { return !e.record; }) == 1);
    const auto event = std::ranges::find_if(entries, [](const ProvenanceEntry& e) { return !e.record; });
    CHECK(event->session == "s-9");
    CHECK(event->what == "renamed ?add2@@YAHHH@Z -> ?add3@@YAHHH@Z");
    CHECK(std::ranges::any_of(entries, [](const ProvenanceEntry& e) { return e.what == "size 15 -> 16" && e.source == SymbolSource::user; }));
    CHECK(symbol_provenance(fx.va("g_counter"), log, nullptr).empty());

    const auto groups = agent_edit_groups(log);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].session == "s-1");
    CHECK(groups[0].records == std::vector<usize>{0, 3});
    CHECK(groups[0].first <= groups[0].last);
    CHECK(groups[1].session == "s-2");
}

TEST_CASE("symbols: reverting agent edits, one at a time or per session") {
    test::FixtureProject fx;
    const u64 add = fx.va("add");
    const u64 global = 0x403030;
    const ChangeOrigin s1{SymbolSource::agent, "s-1", ""};
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = add, .name = std::string("?add2@@YAHHH@Z")}, s1));                                 // 0
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = global, .name = std::string("g_new"), .kind = SymbolKind::data, .size = 4u}, s1));  // 1
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = fx.va("g_counter"), .remove = true}, s1));                                          // 2
    auto log = parse_symbol_log(fx.project.symbol_log());

    // One edit: the rename goes back to the PDB's name.
    auto one = plan_revert(log, std::vector<usize>{0}, current_of(fx.project));
    REQUIRE(one);
    REQUIRE(one->size() == 1);
    CHECK((*one)[0].edit.va == add);
    CHECK((*one)[0].edit.name == "?add@@YAHHH@Z");
    CHECK((*one)[0].edit.kind == SymbolKind::function);
    CHECK((*one)[0].edit.size == 15u);

    // A later change of the same symbol must be reverted first.
    REQUIRE(fx.project.set_symbol(SymbolEdit{.va = add, .name = std::string("?add3@@YAHHH@Z")}, ChangeOrigin{SymbolSource::agent, "s-2", ""}));  // 3
    log = parse_symbol_log(fx.project.symbol_log());
    auto stale = plan_revert(log, std::vector<usize>{0}, current_of(fx.project));
    REQUIRE_FALSE(stale);
    CHECK(stale.error().message.find("revert the later change first") != std::string::npos);
    auto both = plan_revert(log, std::vector<usize>{0, 3}, current_of(fx.project));
    REQUIRE(both);
    CHECK((*both)[0].record == 3);  // newest first
    CHECK((*both)[0].edit.name == "?add2@@YAHHH@Z");
    CHECK((*both)[1].edit.name == "?add@@YAHHH@Z");

    // The whole session s-1, newest first: recreate g_counter, remove g_new, rename add back (after s-2's
    // edit is undone too, since the plan includes it).
    auto session = plan_revert(log, std::vector<usize>{0, 1, 2, 3}, current_of(fx.project));
    REQUIRE(session);
    REQUIRE(session->size() == 4);
    CHECK((*session)[1].record == 2);
    CHECK((*session)[1].edit.name == "?g_counter@@3HA");
    CHECK((*session)[2].edit.remove);
    apply_plan(fx.project, *session);
    const SymbolDb after = symbols_of(fx.project);
    CHECK(after.at(add)->name == "?add@@YAHHH@Z");
    CHECK(after.at(add)->source == SymbolSource::user);  // reverts are user changes
    CHECK(after.at(global) == nullptr);
    REQUIRE(after.at(fx.va("g_counter")));
    CHECK(after.at(fx.va("g_counter"))->kind == SymbolKind::data);
    CHECK(after.at(fx.va("g_counter"))->size == 4);
    // Reverting again finds the symbols changed since (by the revert itself).
    CHECK_FALSE(plan_revert(parse_symbol_log(fx.project.symbol_log()), std::vector<usize>{0}, current_of(fx.project)));
    CHECK_FALSE(plan_revert(log, std::vector<usize>{99}, current_of(fx.project)));
}
