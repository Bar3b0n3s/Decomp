#include "llvm_fixture.hpp"

#include "core/fs.hpp"
#include "core/process.hpp"
#include "matching/toolchain.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

namespace decomp::test {

const std::vector<std::string>& fixture_flags() {
    static const std::vector<std::string> flags = {"/O2", "/Gy", "/GS-", "/GR-", "/EHs-c-"};
    return flags;
}

std::optional<LlvmTools> find_llvm() {
    auto clang_cl = matching::find_clang_cl();
    auto lld_link = matching::find_llvm_tool("lld-link");
    if (!clang_cl || !lld_link) return std::nullopt;
    return LlvmTools{*clang_cl, *lld_link};
}

static std::string target_triple(Arch arch) {
    return arch == Arch::x86 ? "--target=i686-pc-windows-msvc" : "--target=x86_64-pc-windows-msvc";
}

matching::MatchSetup clang_setup(Arch arch, const std::string& clang_cl, const std::filesystem::path& work,
                                 const std::optional<std::filesystem::path>& cache) {
    matching::MatchSetup s;
    s.toolchain.name = arch == Arch::x86 ? "clang-cl-x86" : "clang-cl-x64";
    s.toolchain.kind = matching::ToolchainKind::clang_cl;
    s.toolchain.compiler = clang_cl;
    s.toolchain.flags = {target_triple(arch), "/Zl", "/Brepro"};
    s.flags = fixture_flags();
    s.work_dir = work;
    s.cache_dir = cache;
    return s;
}

std::optional<std::filesystem::path> build_program(Arch arch, const LlvmTools& tools, const std::filesystem::path& dir,
                                                   const std::vector<std::filesystem::path>& sources, const std::string& name,
                                                   const std::vector<std::string>& extra_link_flags,
                                                   const std::vector<std::string>& extra_compile_flags) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string a = arch == Arch::x86 ? "x86" : "x64";
    std::vector<std::string> objs;
    for (const auto& source : sources) {
        auto obj = fs::to_utf8(dir / (fs::to_utf8(source.stem()) + ".obj"));
        ProcessSpec cc;
        cc.argv = {tools.clang_cl, target_triple(arch), "/nologo", "/c", "/Zl", "/Z7", "/Brepro"};
        cc.argv.insert(cc.argv.end(), fixture_flags().begin(), fixture_flags().end());
        cc.argv.insert(cc.argv.end(), extra_compile_flags.begin(), extra_compile_flags.end());
        cc.argv.push_back("/Fo" + obj);
        cc.argv.push_back(fs::to_utf8(source));
        auto r = run_process(cc);
        if (!r || !r->ok()) {
            MESSAGE("clang-cl failed: " << (r ? r->out + r->err : r.error().message));
            return std::nullopt;
        }
        objs.push_back(obj);
    }
    auto exe = dir / (name + ".exe");
    ProcessSpec ld;
    ld.argv = {tools.lld_link, "/nologo", "/nodefaultlib", "/entry:entry", "/subsystem:console", "/debug", "/Brepro",
               "/out:" + fs::to_utf8(exe), "/pdb:" + fs::to_utf8(dir / (name + ".pdb"))};
    ld.argv.insert(ld.argv.end(), extra_link_flags.begin(), extra_link_flags.end());
    ld.argv.insert(ld.argv.end(), objs.begin(), objs.end());
    ld.argv.push_back(fs::to_utf8(fixture(a + "/kernel32.lib")));
    auto r = run_process(ld);
    if (!r || !r->ok()) {
        MESSAGE("lld-link failed: " << (r ? r->out + r->err : r.error().message));
        return std::nullopt;
    }
    return exe;
}

std::optional<std::filesystem::path> build_fixture_program(Arch arch, const LlvmTools& tools, const std::filesystem::path& dir) {
    return build_program(arch, tools, dir, {fixture("src/basic.cpp"), fixture("src/other.cpp")}, "basic");
}

} // namespace decomp::test
