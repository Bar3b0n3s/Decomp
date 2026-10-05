// Static libraries (formats/archive.hpp), their function signatures and matching them against a
// program linked with them (analysis/signatures.hpp, project/library.hpp). The program's PDB is the
// truth for which function is which.

#include "analysis/program.hpp"
#include "analysis/signatures.hpp"
#include "core/fs.hpp"
#include "formats/archive.hpp"
#include "project/library.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>

using namespace decomp;

namespace {

std::string c_name(const char* arch, std::string_view name) { return std::string(arch) == "x86" ? "_" + std::string(name) : std::string(name); }

} // namespace

TEST_CASE("COFF archives: members, long names, the linker's index and import objects") {
    const auto lib = archive::Archive::load(test::fixture("x86/minilib.lib")).value();
    std::vector<std::string> names;
    for (const auto& m : lib.members()) names.push_back(m.name);
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string>{"minilib_counter.obj", "minilib_math.obj", "minilib_text.obj", "minilib_unused.obj"});
    auto add = std::ranges::find(lib.index(), std::string("_lib_add"), &std::pair<std::string, usize>::first);
    REQUIRE(add != lib.index().end());
    CHECK(lib.members()[add->second].name == "minilib_counter.obj");

    const auto imports = archive::Archive::load(test::fixture("x86/kernel32.lib")).value();
    auto exit = std::ranges::find_if(imports.members(), [](const archive::Member& m) { return m.import.has_value(); });
    REQUIRE(exit != imports.members().end());
    CHECK(exit->import->symbol == "_ExitProcess@4");
    CHECK(exit->import->dll == "kernel32.dll");

    CHECK_FALSE(archive::Archive::parse(ByteSpan()));
}

TEST_CASE("library signatures mask what relocations fill in") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const auto sigs = library_signatures(test::fixture(std::string(arch) + "/minilib.lib"), std::string(arch) == "x86" ? Arch::x86 : Arch::x64).value();
        std::map<std::string, const FunctionSignature*> by_name;
        for (const auto& s : sigs) by_name[s.name] = &s;
        for (const char* name : {"lib_add", "lib_mul", "lib_twice", "lib_twice_copy", "lib_strlen", "lib_count_char", "lib_unused"})
            CHECK(by_name.contains(c_name(arch, name)));
        const FunctionSignature* twice = by_name[c_name(arch, "lib_twice")];
        REQUIRE(twice);
        CHECK(twice->library == "minilib.lib");
        CHECK(twice->member == "minilib_math.obj");
        REQUIRE(twice->references.size() == 1);
        CHECK(twice->references[0].symbol == c_name(arch, "lib_add"));
        CHECK(twice->references[0].pc_relative);
        for (u8 k = 0; k < twice->references[0].size; ++k) CHECK_FALSE(twice->mask[twice->references[0].offset + k]);
        const FunctionSignature* add = by_name[c_name(arch, "lib_add")];
        CHECK(std::ranges::any_of(add->references, [&](const SignatureReference& r) { return r.symbol == c_name(arch, "lib_counter"); }));
    }
}

TEST_CASE("library functions are found in a program without its PDB; same-code functions stay ambiguous") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const auto exe = test::fixture(std::string(arch) + "/libuser.exe");
        const Program truth = Program::open(exe).value();
        REQUIRE(truth.pdb_status() == PdbStatus::matched);
        OpenOptions options;
        options.use_pdb = false;
        const Program p = Program::open(exe, options).value();
        const auto sigs = library_signatures(test::fixture(std::string(arch) + "/minilib.lib"), p.arch()).value();
        const auto matches = match_library_functions(p, sigs);
        std::map<std::string, u64> matched;
        std::vector<u64> ambiguous;
        for (const auto& m : matches) {
            if (m.chosen) matched[m.chosen->name] = m.va;
            else ambiguous.push_back(m.va);
        }
        for (const char* name : {"lib_add", "lib_mul", "lib_strlen", "lib_count_char"}) {
            CAPTURE(name);
            REQUIRE(matched.contains(c_name(arch, name)));
            CHECK(matched[c_name(arch, name)] == truth.resolve(c_name(arch, name)));
        }
        CHECK_FALSE(matched.contains(c_name(arch, "lib_unused")));
        // lib_twice and lib_twice_copy have the same code: each fits both names.
        std::ranges::sort(ambiguous);
        std::vector<u64> twins{*truth.resolve(c_name(arch, "lib_twice")), *truth.resolve(c_name(arch, "lib_twice_copy"))};
        std::ranges::sort(twins);
        CHECK(ambiguous == twins);
        // With the PDB's names, every library function is told apart.
        usize named = 0;
        for (const auto& m : match_library_functions(truth, sigs)) named += m.chosen != nullptr;
        CHECK(named == 6);
    }
}

TEST_CASE("decomp lib match names the project's library functions and marks them library") {
    auto dir = fs::TempDir::create("decomp-lib").value();
    // The program without its PDB, as a VC6 target comes.
    const auto exe = dir.path() / "libuser.exe";
    std::filesystem::copy_file(test::fixture("x86/libuser.exe"), exe);
    const Program truth = Program::open(test::fixture("x86/libuser.exe")).value();
    auto p = project::Project::init(dir.path() / "p", exe, std::nullopt, "clang-cl-x86").value();
    const std::vector<std::filesystem::path> libs{test::fixture("x86/minilib.lib")};

    const auto dry = project::match_libraries(p, libs, false).value();
    CHECK(dry.matched == 4);
    CHECK(dry.ambiguous == 2);
    const u64 add = *truth.resolve("_lib_add");
    CHECK(p.function_info(add).status == project::FunctionStatus::unstarted);  // a dry run changes nothing

    const auto r = project::match_libraries(p, libs, true).value();
    CHECK(r.signatures == 7);
    CHECK(r.matched == 4);
    CHECK(r.by_library.at("minilib.lib") == 4);
    std::optional<Symbol> s;
    for (const Symbol& x : p.symbols())
        if (x.va == add) s = x;
    REQUIRE(s);
    CHECK(s->name == "_lib_add");
    CHECK(s->source == SymbolSource::library);
    CHECK(s->size == truth.symbols().at(add)->size);
    CHECK(s->object == "minilib.lib:minilib_counter.obj");
    CHECK(p.function_info(add).status == project::FunctionStatus::library);
    // The program's own function is left alone.
    const u64 entry = *truth.resolve("_entry");
    CHECK(p.function_info(entry).status == project::FunctionStatus::unstarted);

    // Again: the same functions, already named.
    const auto again = project::match_libraries(p, libs, true).value();
    CHECK(again.matched == 4);
    CHECK(again.resized == 0);
}
