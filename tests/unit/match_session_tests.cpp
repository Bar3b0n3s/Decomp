#include "agent/match_session.hpp"
#include "core/fs.hpp"
#include "events/run_state.hpp"
#include "matching/toolchain.hpp"
#include "llvm_fixture.hpp"
#include "project/units.hpp"
#include "test_util.hpp"
#include "viewmodel/symbols_table.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <map>

using namespace decomp;
using namespace decomp::agent;

namespace {

matching::MatchSetup clang_setup(const std::filesystem::path& work) {
    matching::MatchSetup s;
    s.toolchain.name = "clang-cl-x86";
    s.toolchain.kind = matching::ToolchainKind::clang_cl;
    s.toolchain.compiler = matching::find_clang_cl().value_or("clang-cl");
    s.toolchain.flags = {"--target=i686-pc-windows-msvc", "/Zl", "/Brepro"};
    s.flags = {"/O2", "/Gy", "/GS-", "/GR-", "/EHs-c-"};
    s.work_dir = work;
    return s;
}

const char* kAddSource = R"(extern int g_counter;
__declspec(noinline) int add(int a, int b) { return a + b + g_counter; }
)";

} // namespace

TEST_CASE("tool schemas are strict-compatible") {
    auto schemas = MatchSession::tool_schemas();
    CHECK(schemas.size() == 8);
    for (auto it = schemas.begin(); it != schemas.end(); ++it) {
        CAPTURE(it.key());
        const auto& s = *it;
        CHECK(s["type"] == "object");
        CHECK(s["additionalProperties"] == false);
        CHECK(s["required"].size() == s["properties"].size());  // every field required: no optional fields in strict mode
        CHECK_FALSE(MatchSession::tool_description(it.key()).empty());
    }
    CHECK(system_prompt().find("compile_and_diff") != std::string::npos);
}

TEST_CASE("read-only tools and the brief") {
    auto program = Program::open(test::fixture("x86/basic.exe")).value();
    auto tmp = fs::TempDir::create("decomp-session").value();
    MatchSession session(program, nullptr, clang_setup(tmp.path()), *program.resolve("dispatch"));

    auto brief = session.brief();
    CHECK(brief.find("int __cdecl dispatch(int, int)") != std::string::npos);
    CHECK(brief.find("?dispatch@@YAHHH@Z") != std::string::npos);
    CHECK(brief.find("switch: 6 cases") != std::string::npos);
    CHECK(brief.find("calls add: int __cdecl add(int, int)") != std::string::npos);
    CHECK(brief.find("initial bytes: 03 00 00 00") != std::string::npos);  // g_counter = 3
    CHECK(brief.find("clang-cl-x86") != std::string::npos);

    auto dis = session.call("disassemble", Json{{"target", "add"}});
    CHECK_FALSE(dis.is_error);
    CHECK(dis.text.find("add eax, dword ptr [g_counter]") != std::string::npos);
    CHECK(session.call("disassemble", Json{{"target", "nope"}}).is_error);

    auto str = session.call("read_memory", Json{{"address", "0x402010"}, {"count", 0}, {"format", "string"}});
    CHECK(str.text.find("\"hello world\"") != std::string::npos);
    auto table = session.call("read_memory", Json{{"address", "g_table+0x8"}, {"count", 3}, {"format", "i32"}});
    CHECK(table.text.find("[0] 2") != std::string::npos);
    CHECK(table.text.find("[2] 5") != std::string::npos);
    auto ptrs = session.call("read_memory", Json{{"address", "0x40201c"}, {"count", 2}, {"format", "pointer"}});
    CHECK(ptrs.text.find("dispatch") != std::string::npos);
    auto f = session.call("read_memory", Json{{"address", "0x402004"}, {"count", 1}, {"format", "f32"}});
    CHECK(f.text.find("1.5") != std::string::npos);
    auto bytes = session.call("read_memory", Json{{"address", "0x403000"}, {"count", 4}, {"format", "bytes"}});
    CHECK(bytes.text.find("03 00 00 00") != std::string::npos);

    auto sym = session.call("lookup_symbol", Json{{"query", "sum"}});
    CHECK(sym.text.find("sum_array") != std::string::npos);
    CHECK(session.call("lookup_symbol", Json{{"query", "0x401060"}}).text.find("add") != std::string::npos);

    CHECK(session.call("record_note", Json{{"text", "switch has 6 cases"}}).text == "noted");
    CHECK(session.call("record_note", Json{{"text", " "}}).is_error);
    CHECK(session.call("bogus", Json::object()).is_error);
    CHECK(session.status_line(12).find("turns left: 12") != std::string::npos);
}

TEST_CASE("compile_and_diff and submit_result with a real compiler") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-session2").value();
    auto exe = test::build_fixture_program(Arch::x86, *tools, tmp.path() / "target");
    REQUIRE(exe);
    auto program = Program::open(*exe).value();
    events::EventBus bus("run-t");
    events::RunState state;
    bus.subscribe([&](const events::Event& e) { state.apply(e); });
    MatchSession session(program, nullptr, clang_setup(tmp.path()), *program.resolve("add"), &bus, "s1", 0);

    auto wrong = session.call("compile_and_diff", Json{{"source", "extern int g_counter;\n__declspec(noinline) int add(int a, int b) { return a - b + g_counter; }\n"}});
    CHECK_FALSE(wrong.is_error);
    CHECK(wrong.text.find("not matching") != std::string::npos);
    CHECK(session.best_match() > 0);
    CHECK(session.best_match() < 100);

    auto broken = session.call("compile_and_diff", Json{{"source", "int add(int a, int b) { return a + b + nope; }"}});
    CHECK(broken.text.find("compile: FAILED") != std::string::npos);
    CHECK(broken.text.find("nope") != std::string::npos);

    auto rejected = session.call("submit_result", Json{{"outcome", "matched"}, {"source", "int add(int a, int b) { return a * b; }"}, {"reason", ""}});
    CHECK(rejected.is_error);
    CHECK_FALSE(rejected.end_session);

    auto good = session.call("compile_and_diff", Json{{"source", kAddSource}});
    CHECK(good.text.find("MATCHING (byte-exact)") != std::string::npos);
    auto accepted = session.call("submit_result", Json{{"outcome", "matched"}, {"source", kAddSource}, {"reason", ""}});
    CHECK_FALSE(accepted.is_error);
    CHECK(accepted.end_session);
    CHECK(accepted.outcome["outcome"] == "matched");
    CHECK(session.matched());
    CHECK(session.attempts().size() == 5);
    CHECK(state.data().sessions.at("s1")->compiles == 5);
    CHECK(state.data().sessions.at("s1")->best_match == 100.0);

    auto give_up = MatchSession(program, nullptr, clang_setup(tmp.path()), *program.resolve("add"))
                       .call("submit_result", Json{{"outcome", "give_up"}, {"source", ""}, {"reason", "stuck"}});
    CHECK(give_up.end_session);
    CHECK(give_up.outcome["outcome"] == "gave_up");
}

TEST_CASE("session state persists into the project") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-session-project").value();
    REQUIRE(test::build_fixture_program(Arch::x86, *tools, dir.path()));
    auto proj = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
    auto program = proj.open_program().value();
    u64 va = *program.resolve("add");
    {
        MatchSession session(program, &proj, clang_setup(dir.path() / "work"), va);
        // The fixture's PDB gives add its unit: the match goes into the unit's source.
        REQUIRE(session.unit());
        CHECK(session.unit()->name == "basic.obj");
        session.call("record_note", Json{{"text", "uses g_counter"}});
        session.call("compile_and_diff", Json{{"source", kAddSource}});
        CHECK(session.call("submit_result", Json{{"outcome", "matched"}, {"source", kAddSource}, {"reason", ""}}).end_session);
        CHECK(session.written_path() == "src/basic.cpp");
    }
    const Symbol* add = program.symbols().at(va);
    CHECK(project::matched_source_location(proj, *add, project::load_units(proj).value()) == proj.root() / "src" / "basic.cpp");
    CHECK_FALSE(std::filesystem::exists(proj.matched_source_path(*add)));
    CHECK(proj.attempts(*add).size() == 2);
    CHECK(proj.best_source(*add) == std::string(kAddSource));
    // A new session's brief carries the history.
    MatchSession again(program, &proj, clang_setup(dir.path() / "work"), va);
    auto brief = again.brief();
    CHECK(brief.find("uses g_counter") != std::string::npos);
    CHECK(brief.find("best source so far") != std::string::npos);
}

TEST_CASE("a session in a unit: the brief shows the unit, candidates compile in it, and a match keeps it byte-exact") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-session-unit").value();
    REQUIRE(test::build_fixture_program(Arch::x86, *tools, dir.path()));
    auto proj = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
    auto program = proj.open_program().value();
    const u64 add = *program.resolve("add"), read_counter = *program.resolve("read_counter");
    {
        // The unit's source does not exist yet: the function starts it.
        MatchSession session(program, &proj, clang_setup(dir.path() / "work"), add);
        const auto brief = session.brief();
        CHECK(brief.find("# Translation unit\nThis function belongs to the unit basic.obj (12 functions). Its source will be src/basic.cpp "
                         "(C++): no function of the unit is matched yet, so yours starts it.\n") != std::string::npos);
        CHECK(brief.find("The unit's prelude") == std::string::npos);
        CHECK(session.call("submit_result", Json{{"outcome", "matched"}, {"source", kAddSource}, {"reason", ""}}).end_session);
    }
    MatchSession session(program, &proj, clang_setup(dir.path() / "work"), read_counter);
    const auto brief = session.brief();
    CHECK(brief.find("Its source is src/basic.cpp (C++), which holds 1 of them: add.\n") != std::string::npos);
    CHECK(brief.find("```cpp\nextern int g_counter;\n```") != std::string::npos);
    // Byte-exact on its own, but its first #pragma stays in the unit's prelude and turns off optimization
    // for add, which comes first.
    const std::string breaks_add = "#pragma clang optimize off\nextern int g_counter;\n\n#pragma clang optimize on\n"
                                   "__declspec(noinline) int read_counter() { return g_counter; }\n";
    const auto compiled = session.call("compile_and_diff", Json{{"source", breaks_add}});
    CHECK(compiled.text.find("MATCHING (byte-exact)") != std::string::npos);
    CHECK(compiled.text.find("unit: with your source in src/basic.cpp, 1 other function(s) there are not byte-exact: add (") != std::string::npos);
    const auto refused = session.call("submit_result", Json{{"outcome", "matched"}, {"source", breaks_add}, {"reason", ""}});
    CHECK(refused.is_error);
    CHECK_FALSE(refused.end_session);
    CHECK(refused.text.find("these functions of the unit are no longer byte-exact: add (") != std::string::npos);
    // A source that does not define the function at its top level cannot join.
    const auto stray = session.call("compile_and_diff", Json{{"source", "namespace n { int read_counter() { return 0; } }\n"}});
    CHECK(stray.text.find("unit: your source cannot join src/basic.cpp: the source has no definition of read_counter") != std::string::npos);

    const std::string good = "extern int g_counter;\n__declspec(noinline) int read_counter() { return g_counter; }\n";
    CHECK(session.call("submit_result", Json{{"outcome", "matched"}, {"source", good}, {"reason", ""}}).end_session);
    CHECK(session.written_path() == "src/basic.cpp");
    CHECK(fs::read_text(proj.root() / "src" / "basic.cpp").value() ==
          std::format("extern int g_counter;\n\n// FUNCTION: {:#010x}\n__declspec(noinline) int add(int a, int b) {{ return a + b + g_counter; }}\n\n"
                      "// FUNCTION: {:#010x}\n__declspec(noinline) int read_counter() {{ return g_counter; }}\n",
                      add, read_counter));
    // The write is recorded against the unit and the function.
    const auto changes = proj.changes();
    REQUIRE_FALSE(changes.empty());
    CHECK(changes.back()["path"] == "src/basic.cpp");
    CHECK(changes.back()["unit"] == "basic.obj");
    CHECK(changes.back()["va"] == read_counter);
    CHECK(changes.back()["reason"] == "verified match");
    // The best source of each function is its own translation unit, not the unit's.
    CHECK(proj.best_source(*program.symbols().at(read_counter)) == good);
    const auto verified = project::verify_unit_sources(proj, program, clang_setup(dir.path() / "work")).value();
    REQUIRE(verified.size() == 1);
    CHECK(verified[0].verification.functions.size() == 2);
    CHECK(verified[0].verification.all_byte_exact());
}

TEST_CASE("set_symbol and define_type: approvals, provenance, checks and revert") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-session-tools").value();
    REQUIRE(test::build_fixture_program(Arch::x86, *tools, dir.path()));
    auto proj = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
    auto program = proj.open_program().value();
    events::EventBus bus("run-tools");
    events::RunState state;
    bus.subscribe([&](const events::Event& e) { state.apply(e); });
    auto setup = clang_setup(dir.path() / "work");
    setup.include_dirs = {proj.root() / "include"};
    MatchSession session(program, &proj, setup, *program.resolve("dispatch"), &bus, "s1", 0);
    auto gate = std::make_shared<ApprovalGate>(&bus);
    for (std::string_view action : approval_actions()) gate->set_policy(std::string(action), ApprovalPolicy::automatic);
    session.set_approvals(gate);
    auto set_symbol = [&](const char* address, const char* name, const char* kind = "", int size = 0) {
        return session.call("set_symbol", Json{{"address", address}, {"name", name}, {"kind", kind}, {"size", size}, {"reason", "seen in dispatch"}});
    };

    SUBCASE("set_symbol") {
        const u64 other = *program.resolve("other_value");
        auto renamed = set_symbol("other_value", "lookup_other");
        CHECK_FALSE(renamed.is_error);
        CHECK(renamed.text.find("rename other_value to lookup_other") != std::string::npos);
        const auto symbols = proj.symbols();
        const auto it = std::ranges::find(symbols, other, &Symbol::va);
        REQUIRE(it != symbols.end());
        CHECK(it->name == "lookup_other");
        CHECK(it->source == SymbolSource::agent);
        // Provenance: the log names the session and the reason; the run's events have it.
        const auto log = proj.symbol_log();
        REQUIRE_FALSE(log.empty());
        CHECK(log.back()["session"] == "s1");
        CHECK(log.back()["reason"] == "seen in dispatch");
        REQUIRE(state.data().symbol_changes.size() == 1);
        CHECK(state.data().symbol_changes.back().change.new_name == "lookup_other");
        CHECK(state.data().symbol_changes.back().change.source == "agent");
        // A new data symbol at an address without one.
        CHECK_FALSE(set_symbol("0x403020", "g_extra", "data", 4).is_error);
        // Refused: the session's own function, a name in use, nothing to change, no reason, an unknown kind.
        CHECK(set_symbol("dispatch", "dispatch2").is_error);
        CHECK(set_symbol("read_counter", "add").is_error);
        CHECK(set_symbol("read_counter", "").is_error);
        CHECK(session.call("set_symbol", Json{{"address", "read_counter"}, {"name", "x"}, {"kind", ""}, {"size", 0}, {"reason", ""}}).is_error);
        CHECK(set_symbol("read_counter", "", "import").is_error);
        // Refused: a matched function, and a name a verified source uses.
        REQUIRE(proj.update_function(*program.resolve("add"), {project::FunctionStatus::matched, 100, 1, 0}));
        CHECK(set_symbol("add", "add2").is_error);
        REQUIRE(fs::write_text(proj.root() / "src" / "functions" / "read_counter_401070.cpp",
                               "extern int g_table[8];\nint read_counter() { return g_table[0]; }\n"));
        const auto used = set_symbol("g_table", "g_fib");
        CHECK(used.is_error);
        CHECK(used.text.find("src/functions/read_counter_401070.cpp") != std::string::npos);
        // Denied by policy: nothing changes.
        gate->set_policy(std::string(kSetSymbolAction), ApprovalPolicy::deny);
        const auto denied = set_symbol("sum_array", "sum_ints");
        CHECK(denied.is_error);
        CHECK(denied.text.find("not allowed by this run's policy") != std::string::npos);
        CHECK(program.resolve("sum_array"));
        CHECK(std::ranges::none_of(proj.symbols(), [](const Symbol& s) { return s.name == "sum_ints"; }));

        // Revert, as `decomp symbols revert` and the Symbols view do.
        const auto records = vm::parse_symbol_log(proj.symbol_log());
        std::map<u64, vm::SymbolState> current;
        for (const Symbol& s : proj.symbols()) current.emplace(s.va, vm::symbol_state(s));
        const usize first = records.size() - 2;  // the rename (the data symbol came after it)
        const auto plan = vm::plan_revert(records, std::vector<usize>{first}, [&](u64 va) -> std::optional<vm::SymbolState> {
            auto c = current.find(va);
            return c == current.end() ? std::nullopt : std::optional(c->second);
        });
        REQUIRE(plan);
        REQUIRE(plan->size() == 1);
        REQUIRE(proj.set_symbol(plan->front().edit, project::ChangeOrigin{SymbolSource::user, "", "revert"}));
        const auto reverted = proj.symbols();
        CHECK(std::ranges::find(reverted, other, &Symbol::va)->name == program.symbols().at(other)->name);
    }

    SUBCASE("define_type") {
        auto define = [&](const char* name, const char* declaration, const char* header = "") {
            return session.call("define_type", Json{{"name", name}, {"declaration", declaration}, {"header", header}, {"reason", "fields at +0, +4"}});
        };
        const auto made = define("Point", "struct Point {\n    int x;\n    int y;\n};");
        CHECK_FALSE(made.is_error);
        CHECK(made.text.find("#include \"types.h\"") != std::string::npos);
        const auto header = proj.root() / "include" / "types.h";
        CHECK(fs::read_text(header).value() == "#pragma once\n\nstruct Point {\n    int x;\n    int y;\n};\n");
        auto changes = proj.changes();
        REQUIRE_FALSE(changes.empty());
        CHECK(changes.back()["path"] == "include/types.h");
        CHECK(changes.back()["source"] == "agent");
        CHECK(changes.back()["reason"] == "fields at +0, +4");
        REQUIRE_FALSE(state.data().files_written.empty());
        CHECK(state.data().files_written.back().file.path == "include/types.h");

        // Refused: no such type in the declaration, functions, a bad header name, a header that does not compile.
        CHECK(define("Player", "struct Other { int a; };").is_error);
        CHECK(define("Player", "struct Player { int hp; };\nint f() { return 0; }").is_error);
        CHECK(define("Player", "struct Player { int hp; };", "../player.h").is_error);
        const auto broken = define("Player", "struct Player { int hp; nonsense; };");
        CHECK(broken.is_error);
        CHECK(broken.text.find("does not compile") != std::string::npos);
        CHECK(fs::read_text(header).value().find("Player") == std::string::npos);

        // A verified source that includes the header and defines Player itself: adding Player there would
        // break it, while another header is fine.
        REQUIRE(proj.update_function(*program.resolve("read_counter"), {project::FunctionStatus::matched, 100, 1, 0}));
        REQUIRE(fs::write_text(proj.root() / "src" / "functions" / "read_counter_401070.cpp",
                               "#include \"types.h\"\nstruct Player { int hp; };\nextern int g_counter;\n"
                               "__declspec(noinline) int read_counter() { return g_counter; }\n"));
        const auto breaks = define("Player", "struct Player { int hp; float speed; };");
        CHECK(breaks.is_error);
        CHECK(breaks.text.find("read_counter") != std::string::npos);
        CHECK_FALSE(define("Player", "struct Player { int hp; float speed; };", "game/player.h").is_error);
        CHECK(std::filesystem::exists(proj.root() / "include" / "game" / "player.h"));

        // Replacing a definition keeps its place.
        CHECK_FALSE(define("Point", "struct Point {\n    int x;\n    int y;\n    int z;\n};").is_error);
        const std::string replaced = fs::read_text(header).value();
        CHECK(replaced == "#pragma once\n\nstruct Point {\n    int x;\n    int y;\n    int z;\n};\n");

        // Revert the replacement: the first definition is back.
        changes = proj.changes();
        REQUIRE(proj.revert_change(changes.back(), project::ChangeOrigin{SymbolSource::user, "", "revert"}));
        CHECK(fs::read_text(header).value() == "#pragma once\n\nstruct Point {\n    int x;\n    int y;\n};\n");
        // Denied by policy.
        gate->set_policy(std::string(kDefineTypeAction), ApprovalPolicy::deny);
        CHECK(define("Rect", "struct Rect { Point a, b; };").is_error);
        CHECK(fs::read_text(header).value().find("Rect") == std::string::npos);
    }
}
