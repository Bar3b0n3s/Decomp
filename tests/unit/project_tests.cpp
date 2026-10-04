#include "core/fs.hpp"
#include "core/json.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <fstream>

using namespace decomp;
using namespace decomp::project;

TEST_CASE("symbols.txt line codec round-trips") {
    Symbol s;
    s.va = 0x401000;
    s.kind = SymbolKind::function;
    s.name = "?Hit@Player@@QAEXH@Z";
    s.pdb_name = "Player::Hit";
    s.size = 0x1e;
    s.source = SymbolSource::pdb_public;
    FunctionInfo info{FunctionStatus::nonmatching, 87.5, 3, 0.4321};
    auto line = format_symbol_line(s, &info);
    CHECK(line == "0x00401000 function ?Hit@Player@@QAEXH@Z size=0x1e pdb=Player::Hit source=pdb_public status=nonmatching best=87.5 attempts=3 cost=0.4321");
    auto [parsed, parsed_info] = parse_symbol_line(line).value();
    CHECK(parsed.va == s.va);
    CHECK(parsed.name == s.name);
    CHECK(parsed.pdb_name == s.pdb_name);
    CHECK(parsed.size == s.size);
    CHECK(parsed.source == SymbolSource::pdb_public);
    REQUIRE(parsed_info);
    CHECK(parsed_info->status == FunctionStatus::nonmatching);
    CHECK(parsed_info->attempts == 3);

    Symbol spaced;
    spaced.va = 0x402010;
    spaced.kind = SymbolKind::string;
    spaced.name = "my string";
    spaced.source = SymbolSource::user;
    auto round = parse_symbol_line(format_symbol_line(spaced, nullptr)).value();
    CHECK(round.first.name == "my string");
    CHECK_FALSE(round.second);

    CHECK_FALSE(parse_symbol_line("0x401000 function"));
    CHECK_FALSE(parse_symbol_line("zz function f"));
    CHECK_FALSE(parse_symbol_line("0x401000 weird f"));
    CHECK_FALSE(parse_symbol_line("0x401000 function f status=bogus"));
}

TEST_CASE("project init, load, symbol overrides and function state") {
    auto dir = fs::TempDir::create("decomp-project").value();
    auto root = dir.path() / "proj";
    auto target = dir.path() / "basic.exe";
    std::filesystem::copy_file(test::fixture("x86/basic.exe"), target);
    std::filesystem::copy_file(test::fixture("x86/basic.pdb"), dir.path() / "basic.pdb");

    auto p = Project::init(root, target, std::nullopt, "clang-cl-x86").value();
    CHECK(p.config().target == "../basic.exe");
    CHECK(p.config().pdb == "../basic.pdb");
    CHECK(p.config().toolchain == "clang-cl-x86");
    CHECK(p.config().target_sha1.size() == 40);
    CHECK(std::filesystem::exists(root / "symbols.txt"));
    CHECK(std::filesystem::exists(root / "include"));
    CHECK_FALSE(Project::init(root, target, std::nullopt, "").has_value());  // already initialised

    // A user rename in symbols.txt wins over the PDB name.
    auto text = fs::read_text(root / "symbols.txt").value();
    auto pos = text.find("0x00401060 function ?add@@YAHHH@Z");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, std::string("0x00401060 function ?add@@YAHHH@Z").size(), "0x00401060 function my_add");
    REQUIRE(fs::write_text(root / "symbols.txt", text).has_value());
    auto reloaded = Project::find(fs::to_utf8(root / "include")).value();  // found by searching upwards
    auto program = reloaded.open_program().value();
    CHECK(program.symbols().at(0x401060)->name == "my_add");
    CHECK(program.resolve("my_add") == 0x401060u);

    REQUIRE(reloaded.update_function(0x401060, {FunctionStatus::matched, 100, 2, 0.12}).has_value());
    auto again = Project::load(root).value();
    CHECK(again.function_info(0x401060).status == FunctionStatus::matched);
    CHECK(again.function_info(0x401060).attempts == 2);
    CHECK(again.function_info(0x401070).status == FunctionStatus::unstarted);

    const Symbol* add = program.symbols().at(0x401060);
    CHECK(safe_function_name(*add) == "add_401060");
    REQUIRE(again.record_attempt(*add, Json{{"n", 1}, {"match", 50.0}}).has_value());
    REQUIRE(again.record_attempt(*add, Json{{"n", 2}, {"match", 100.0}}).has_value());
    CHECK(again.attempts(*add).size() == 2);
    REQUIRE(again.save_best_source(*add, "int add(int a, int b);").has_value());
    CHECK(again.best_source(*add) == "int add(int a, int b);");
    REQUIRE(again.append_note(*add, "uses g_counter").has_value());
    CHECK(again.notes(*add).find("uses g_counter") != std::string::npos);
    REQUIRE(again.write_matched_source(*add, "// matched\n").has_value());
    CHECK(std::filesystem::exists(root / "src" / "functions" / "add_401060.cpp"));
}

TEST_CASE("Project::find reports a helpful error outside a project") {
    auto dir = fs::TempDir::create("decomp-noproject").value();
    auto r = Project::find(fs::to_utf8(dir.path()));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::not_found);
    CHECK(r.error().message.find("decomp init") != std::string::npos);
}

TEST_CASE("a changed target binary is refused") {
    auto dir = fs::TempDir::create("decomp-sha").value();
    auto target = dir.path() / "basic.exe";
    std::filesystem::copy_file(test::fixture("x86/basic.exe"), target);
    std::filesystem::copy_file(test::fixture("x86/basic.pdb"), dir.path() / "basic.pdb");
    auto p = Project::init(dir.path() / "proj", target, std::nullopt, "clang-cl-x86").value();
    REQUIRE(p.open_program());
    // Append a byte: the image still loads, but it is no longer the binary the project describes.
    {
        std::ofstream out(target, std::ios::app | std::ios::binary);
        out.put('\0');
    }
    auto changed = p.open_program();
    REQUIRE_FALSE(changed);
    CHECK(changed.error().message.find("has changed") != std::string::npos);
}
