// Exception-handling tables (analysis/eh.hpp) and the code only exceptions reach, which discovery keeps
// in its function (analysis/discovery.hpp). The fixture is the corpus's eh.cpp, seh.c and eh_rt.c built
// with clang-cl; its PDB is the truth.

#include "analysis/annotate.hpp"
#include "analysis/bounds.hpp"
#include "analysis/eh.hpp"
#include "analysis/program.hpp"
#include "formats/map.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

namespace {

// The values of a function's 32-bit immediates (registration code stores the handler and its table).
std::vector<u64> immediates(const Program& p, u64 start, u64 end) {
    std::vector<u64> out;
    const auto bytes = p.image().view(start, static_cast<usize>(end - start));
    if (!bytes) return out;
    for (const auto& ins : p.decoder().decode_all(*bytes, start))
        for (const auto& f : ins.fields)
            if (f.kind == x86::FieldKind::imm && f.size == 4) out.push_back(static_cast<u32>(f.raw));
    return out;
}

struct Extent {
    u64 start = 0, end = 0;
    bool contains(u64 va) const { return va > start && va < end; }
};

Extent extent(const Program& p, const char* name) {
    const Symbol* s = p.symbols().find(name);
    REQUIRE(s);
    return {s->va, s->va + s->size};
}

} // namespace

TEST_CASE("x86 C++ EH: the handler stub's FuncInfo lists the catch blocks and the unwind code") {
    const Program p = Program::open(test::fixture("x86/eh.exe")).value();
    REQUIRE(p.pdb_status() == PdbStatus::matched);

    const Extent caught = extent(p, "?eh_catch@@YAHH@Z");
    std::optional<CxxFuncInfo> info;
    for (u64 v : immediates(p, caught.start, caught.end))
        if (auto at = cxx_stub_funcinfo(p.image(), p.decoder(), v)) info = read_cxx_funcinfo(p.image(), *at);
    REQUIRE(info);
    CHECK(info->magic == kFuncInfoMagic3);
    CHECK(info->try_blocks == 1);
    REQUIRE(info->catch_blocks.size() == 3);  // Failure, int and ...
    for (u64 c : info->catch_blocks) CHECK(caught.contains(c));
    CHECK(info->unwind_actions.empty());

    const Extent guarded = extent(p, "?eh_guarded@@YAHH@Z");
    std::optional<CxxFuncInfo> unwinds;
    for (u64 v : immediates(p, guarded.start, guarded.end))
        if (auto at = cxx_stub_funcinfo(p.image(), p.decoder(), v)) unwinds = read_cxx_funcinfo(p.image(), *at);
    REQUIRE(unwinds);
    CHECK(unwinds->try_blocks == 0);
    REQUIRE(unwinds->unwind_actions.size() == 1);  // the Guard's destructor
    CHECK(guarded.contains(unwinds->unwind_actions[0]));

    // Not a stub, not a FuncInfo.
    CHECK_FALSE(cxx_stub_funcinfo(p.image(), p.decoder(), guarded.start));
    CHECK_FALSE(read_cxx_funcinfo(p.image(), guarded.start));
}

TEST_CASE("x86 SEH: the scope table lists the filter and the __except block") {
    const Program p = Program::open(test::fixture("x86/eh.exe")).value();
    const Extent nested = extent(p, "seh_nested");
    std::optional<ScopeTable> table;
    for (u64 v : immediates(p, nested.start, nested.end))
        if (auto t = read_scope_table(p.image(), v)) table = t;
    REQUIRE(table);
    CHECK_FALSE(table->cookies);  // clang registers _except_handler3
    REQUIRE(table->entries.size() == 1);
    const ScopeEntry& e = table->entries[0];
    CHECK(e.enclosing == -1);
    CHECK(e.filter == p.symbols().find("?filt$0@0@seh_nested@@")->va);  // clang makes a function of the filter
    CHECK(nested.contains(e.handler));
    CHECK_FALSE(read_scope_table(p.image(), nested.start));
}

TEST_CASE("discovery keeps catch blocks, unwind code and __except blocks in their function") {
    OpenOptions options;
    options.use_pdb = false;
    const Program p = Program::open(test::fixture("x86/eh.exe"), options).value();
    const auto truth = pdb_function_bounds(test::fixture("x86/eh.pdb"), p.image().image_base(), p.image()).value();
    const auto c = compare_bounds(truth, function_bounds(p.symbols(), p.image()), p.image(), p.decoder());
    for (const auto& m : c.mismatches)
        if (m.kind != BoundsMismatch::Kind::extra) MESSAGE(to_string(m.kind), " ", m.start, " ", m.name);
    CHECK(c.exact == c.truth);
    // The handler stubs (`mov eax, offset FuncInfo; jmp __CxxFrameHandler3`) have no procedure in the PDB.
    for (const auto& m : c.mismatches) {
        CHECK(m.kind == BoundsMismatch::Kind::extra);
        CHECK(cxx_stub_funcinfo(p.image(), p.decoder(), m.start));
    }
}

TEST_CASE("listings say which code exceptions reach: catch clauses, __except and __finally blocks") {
    const Program p = Program::open(test::fixture("x86/eh.exe")).value();
    const auto caught = annotate_function(p, p.symbols().find("?eh_catch@@YAHH@Z")->va).value();
    REQUIRE(caught.exception_handling.size() == 1);
    const std::string& line = caught.exception_handling[0];
    CHECK(line.find("handler stub __ehhandler$?eh_catch@@YAHH@Z") != std::string::npos);
    CHECK(line.find("1 try block; catch (Failure&) at loc_") != std::string::npos);  // clang leaves out the const
    CHECK(line.find("catch (int) at loc_") != std::string::npos);
    CHECK(line.find("catch (...) at loc_") != std::string::npos);
    const std::string text = to_text(caught);
    CHECK(text.find("; eh:       C++ exception handling") != std::string::npos);
    CHECK(text.find("; catch (int) (try block 0)") != std::string::npos);
    CHECK(text.find("__ehhandler$?eh_catch@@YAHH@Z") != text.rfind("; eh:"));  // in the registration too

    const auto nested = annotate_function(p, p.symbols().find("seh_nested")->va).value();
    REQUIRE(nested.exception_handling.size() == 1);
    CHECK(nested.exception_handling[0].starts_with("__try 0: __except at loc_"));
    CHECK(nested.exception_handling[0].ends_with("filter at ?filt$0@0@seh_nested@@"));
    CHECK(to_text(nested).find("__sehtable$_seh_nested") != std::string::npos);

    // MSVC's layout (the idiom fixture): a catch (...) block in the function, nested __try blocks.
    const Program idioms = Program::open(test::fixture("x86/idioms.exe")).value();
    const auto m = map::load(test::fixture("x86/idioms.map")).value();
    auto va_of = [&](std::string_view name) {
        auto e = std::ranges::find(m.entries, std::string(name), &map::Entry::name);
        REQUIRE(e != m.entries.end());
        return e->va;
    };
    const auto catcher = annotate_function(idioms, va_of("_eh_catcher")).value();
    REQUIRE(catcher.exception_handling.size() == 1);
    CHECK(catcher.exception_handling[0].find("catch (...) at loc_") != std::string::npos);
    const auto seh = annotate_function(idioms, va_of("_seh_user")).value();
    REQUIRE(seh.exception_handling.size() == 2);
    CHECK(seh.exception_handling[0].starts_with("__try 0: __except at loc_"));
    CHECK(seh.exception_handling[1].starts_with("__try 1 in __try 0: __finally at loc_"));
    CHECK(to_text(seh).find("; __finally block (__try 1)") != std::string::npos);

    CatchHandler h;
    h.type = 0;
    CHECK(catch_clause(p.image(), h) == "catch (...)");
}
