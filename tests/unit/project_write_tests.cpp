#include "core/fs.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <thread>
#include <vector>

using namespace decomp;
using namespace decomp::project;

namespace {

struct Fixture {
    fs::TempDir dir = fs::TempDir::create("decomp-pw").value();
    std::filesystem::path root = dir.path() / "p";
    Project project;

    Fixture() {
        std::filesystem::copy_file(test::fixture("x86/basic.exe"), dir.path() / "basic.exe");
        std::filesystem::copy_file(test::fixture("x86/basic.pdb"), dir.path() / "basic.pdb");
        project = Project::init(root, dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
    }
};

} // namespace

TEST_CASE("modify_function: concurrent updates from threads and separate project objects lose nothing") {
    Fixture fx;
    Project other = Project::load(fx.root).value();  // a second object, as another process would have
    const std::vector<u64> vas = {0x401060, 0x4010f0, 0x401000};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            Project& p = t % 2 ? other : fx.project;
            for (int i = 0; i < 25; ++i) {
                auto r = p.modify_function(vas[static_cast<usize>(i) % vas.size()], [](FunctionInfo& f) {
                    ++f.attempts;
                    f.cost_usd += 0.01;
                });
                REQUIRE(r);
            }
        });
    for (auto& t : threads) t.join();
    int total = 0;
    auto reloaded = Project::load(fx.root).value();
    for (u64 va : vas) total += reloaded.function_info(va).attempts;
    CHECK(total == 200);
    // The in-memory views agree with the file once refreshed.
    CHECK(fx.project.reload_if_changed().has_value());
    int seen = 0;
    for (u64 va : vas) seen += fx.project.function_info(va).attempts;
    CHECK(seen == 200);
    auto infos = fx.project.function_infos();
    CHECK(infos->size() >= 3);
}

TEST_CASE("set_symbol renames, creates and removes symbols with an audit trail") {
    Fixture fx;
    const u64 version = fx.project.version();
    auto renamed = fx.project.set_symbol({.va = 0x401060, .name = std::string("my_add")}, {SymbolSource::user, "", "clearer name"}).value();
    REQUIRE(renamed.before);
    REQUIRE(renamed.after);
    CHECK(renamed.after->name == "my_add");
    CHECK(renamed.after->source == SymbolSource::user);
    CHECK(fx.project.version() > version);

    auto created = fx.project.set_symbol({.va = 0x401234, .name = std::string("helper_thunk"), .size = 5u}, {SymbolSource::agent, "s-1", "binding"});
    REQUIRE(created);
    CHECK_FALSE(created->before);
    CHECK(created->after->size == 5);
    CHECK_FALSE(fx.project.set_symbol({.va = 0x401235}, {}));  // a new symbol needs a name
    CHECK(fx.project.set_symbol({.va = 0x401234, .remove = true}, {}).has_value());
    CHECK_FALSE(fx.project.set_symbol({.va = 0x401234, .remove = true}, {}));

    auto program = fx.project.open_program().value();
    CHECK(program.symbols().at(0x401060)->name == "my_add");
    CHECK(program.resolve("my_add") == 0x401060u);

    const auto log = fx.project.symbol_log();
    REQUIRE(log.size() == 3);
    CHECK(log[0]["before"]["name"] == "?add@@YAHHH@Z");
    CHECK(log[0]["after"]["name"] == "my_add");
    CHECK(log[1]["source"] == "agent");
    CHECK(log[1]["session"] == "s-1");
    CHECK(log[2]["after"].is_null());
}

TEST_CASE("matched sources keep the content they replace") {
    Fixture fx;
    auto program = fx.project.open_program().value();
    const Symbol& add = *program.symbols().at(0x401060);
    auto first = fx.project.write_matched_source(add, "int add(int a, int b) { return a + b; }\n", {SymbolSource::agent, "s-1", "verified match"}).value();
    CHECK_FALSE(first.previous_sha1);
    CHECK(first.sha1.size() == 40);
    auto second = fx.project.write_matched_source(add, "// v2\n", {SymbolSource::user, "", "manual"}).value();
    REQUIRE(second.previous_sha1);
    CHECK(*second.previous_sha1 == first.sha1);
    CHECK(fx.project.read_blob(first.sha1).value() == "int add(int a, int b) { return a + b; }\n");
    CHECK_FALSE(fx.project.read_blob("../../etc/passwd"));
    const auto changes = fx.project.changes();
    REQUIRE(changes.size() == 2);
    CHECK(changes[0]["source"] == "agent");
    CHECK(changes[1]["previous_sha1"] == first.sha1);
    CHECK(changes[1]["path"].get<std::string>().find("add_401060.cpp") != std::string::npos);
}

TEST_CASE("revert_change restores the replaced content or removes a new file") {
    Fixture fx;
    auto program = fx.project.open_program().value();
    const Symbol& add = *program.symbols().at(0x401060);
    REQUIRE(fx.project.update_function(0x401060, {FunctionStatus::matched, 100, 2, 0.5}));
    const std::string v1 = "int add(int a, int b) { return a + b; }\n";
    fx.project.write_matched_source(add, v1, {SymbolSource::agent, "s-1", "verified match"}).value();
    fx.project.write_matched_source(add, "// v2\n", {SymbolSource::agent, "s-2", "verified match"}).value();
    auto changes = fx.project.changes();
    REQUIRE(changes.size() == 2);
    // Only the latest write of a file can be reverted: the older one no longer matches the file.
    CHECK_FALSE(fx.project.revert_change(changes[0], {SymbolSource::user, "", ""}));
    REQUIRE(fx.project.revert_change(changes[1], {SymbolSource::user, "", ""}));
    const auto path = fx.project.matched_source_path(add);
    CHECK(fs::read_text(path).value() == v1);
    CHECK(fx.project.function_info(0x401060).status == FunctionStatus::matched);  // the restored source is a match too
    changes = fx.project.changes();
    REQUIRE(changes.size() == 3);
    CHECK(changes[2]["reason"] == "revert");
    CHECK(changes[2]["source"] == "user");
    // Reverting the first write removes the file and the match.
    REQUIRE(fx.project.revert_change(changes[0], {SymbolSource::user, "", ""}));
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(fx.project.function_info(0x401060).status == FunctionStatus::nonmatching);
    CHECK(fx.project.function_info(0x401060).best_match == 100);
    // The removed content is kept as a blob, like any replaced content.
    CHECK(fx.project.read_blob(changes[0]["sha1"].get<std::string>()).value() == v1);
    CHECK_FALSE(fx.project.revert_change(Json{{"path", "x"}}, {}));
}

TEST_CASE("target status reports SHA-1 and PDB state; one active run per project") {
    Fixture fx;
    auto program = fx.project.open_program().value();
    auto status = fx.project.target_status(program);
    CHECK(status.sha1_ok);
    CHECK(status.pdb == PdbStatus::matched);

    {
        std::ofstream out(fx.dir.path() / "basic.exe", std::ios::app | std::ios::binary);
        out.put('\0');
    }
    CHECK_FALSE(fx.project.open_program());
    auto changed = fx.project.open_program(/*verify_target=*/false).value();
    auto changed_status = fx.project.target_status(changed);
    CHECK_FALSE(changed_status.sha1_ok);
    CHECK(changed_status.actual_sha1 != changed_status.expected_sha1);

    auto lock = fx.project.try_lock_active_run().value();
    REQUIRE(lock.has_value());
    CHECK_FALSE(Project::load(fx.root).value().try_lock_active_run().value().has_value());
    lock->release();
    CHECK(fx.project.try_lock_active_run().value().has_value());
}

TEST_CASE("Program::with_symbols shares the image and starts fresh analysis") {
    auto program = Program::open(test::fixture("x86/basic.exe")).value();
    CHECK(program.pdb_status() == PdbStatus::matched);
    SymbolDb db = program.symbols();
    db.rename(0x401060, "renamed_add", SymbolSource::user);
    Program next = program.with_symbols(std::move(db));
    CHECK(&next.image() == &program.image());
    CHECK(next.symbols().at(0x401060)->name == "renamed_add");
    CHECK(program.symbols().at(0x401060)->name != "renamed_add");
    CHECK(next.function_extent(0x401060).has_value());
    CHECK_FALSE(next.callers_of(0x401060).empty());

    auto no_pdb_dir = fs::TempDir::create("decomp-nopdb").value();
    std::filesystem::copy_file(test::fixture("x86/basic.exe"), no_pdb_dir.path() / "basic.exe");
    auto bare = Program::open(no_pdb_dir.path() / "basic.exe").value();
    CHECK(bare.pdb_status() == PdbStatus::absent);
}

TEST_CASE("modify_functions changes many functions in one write; save_notes replaces the notes") {
    Fixture fx;
    Project other = Project::load(fx.root).value();
    REQUIRE(fx.project.update_function(0x401060, FunctionInfo{FunctionStatus::nonmatching, 62.5, 3, 0.25}));
    const u64 before = fx.project.version();
    const std::vector<u64> vas = {0x401060, 0x4010f0, 0x401000};
    REQUIRE(fx.project.modify_functions(vas, [](u64, FunctionInfo& info) { info.status = FunctionStatus::skipped; }));
    CHECK(fx.project.version() == before + 1);  // one rewrite of symbols.txt
    const FunctionInfo kept = fx.project.function_info(0x401060);
    CHECK(kept.status == FunctionStatus::skipped);
    CHECK(kept.best_match == 62.5);  // history stays
    CHECK(kept.attempts == 3);
    // Another project object (another process) sees the change in the file.
    CHECK(other.reload_if_changed().value());
    for (u64 va : vas) CHECK(other.function_info(va).status == FunctionStatus::skipped);
    CHECK(fx.project.modify_functions({}, [](u64, FunctionInfo&) {}));
    CHECK(fx.project.version() == before + 1);  // nothing to write

    const Program program = fx.project.open_program().value();
    const Symbol& add = *program.symbols().at(0x401060);
    REQUIRE(fx.project.append_note(add, "first"));
    CHECK(fx.project.notes(add).find("first") != std::string::npos);
    REQUIRE(fx.project.save_notes(add, "- edited by hand\n"));
    CHECK(fx.project.notes(add) == "- edited by hand\n");
    const Symbol& mix = *program.symbols().at(0x4011a0);
    REQUIRE(fx.project.save_notes(mix, "new notes"));  // a function without notes yet
    CHECK(fx.project.notes(mix) == "new notes");
}
