#include "agent/match_session.hpp"
#include "core/fs.hpp"
#include "events/run_state.hpp"
#include "matching/toolchain.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

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
    CHECK(schemas.size() == 6);
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
    CHECK(state.data().sessions.at("s1").compiles == 5);
    CHECK(state.data().sessions.at("s1").best_match == 100.0);

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
        session.call("record_note", Json{{"text", "uses g_counter"}});
        session.call("compile_and_diff", Json{{"source", kAddSource}});
        CHECK(session.call("submit_result", Json{{"outcome", "matched"}, {"source", kAddSource}, {"reason", ""}}).end_session);
    }
    const Symbol* add = program.symbols().at(va);
    CHECK(std::filesystem::exists(proj.matched_source_path(*add)));
    CHECK(proj.attempts(*add).size() == 2);
    CHECK(proj.best_source(*add) == std::string(kAddSource));
    // A new session's brief carries the history.
    MatchSession again(program, &proj, clang_setup(dir.path() / "work"), va);
    auto brief = again.brief();
    CHECK(brief.find("uses g_counter") != std::string::npos);
    CHECK(brief.find("best source so far") != std::string::npos);
}
