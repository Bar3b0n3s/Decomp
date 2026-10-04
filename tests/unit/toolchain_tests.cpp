#include "core/fs.hpp"
#include "matching/toolchain.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::matching;

TEST_CASE("diagnostics parsing: MSVC, clang-cl and GCC formats") {
    auto d = parse_diagnostics(
        "candidate.cpp(12) : error C2065: 'x' : undeclared identifier\n"
        "candidate.cpp(14,5): warning C4244: '=': conversion from 'double' to 'int'\n"
        "C:\\p\\candidate.cpp(2,52): error: use of undeclared identifier 'undeclared'\n"
        "    2 | int add(int a) { return undeclared; }\n"
        "candidate.cpp:7:3: error: expected ';' after expression\n"
        "candidate.cpp:9: note: something\n"
        "1 error generated.\n");
    REQUIRE(d.size() == 5);
    CHECK(d[0].line == 12);
    CHECK(d[0].column == 0);
    CHECK(d[0].severity == "error");
    CHECK(d[0].code == "C2065");
    CHECK(d[0].message == "'x' : undeclared identifier");
    CHECK(d[1].severity == "warning");
    CHECK(d[1].column == 5);
    CHECK(d[2].file == "C:\\p\\candidate.cpp");
    CHECK(d[2].code.empty());
    CHECK(d[2].message == "use of undeclared identifier 'undeclared'");
    CHECK(d[3].line == 7);
    CHECK(d[3].column == 3);
    CHECK(d[4].severity == "note");
    CHECK(format_diagnostics(d).find("line 12: error C2065:") != std::string::npos);
}

TEST_CASE("toolchain JSON round trip and validation") {
    auto j = parse_json(R"({"kind": "msvc", "compiler": "C:/VC6/VC98/Bin/CL.EXE",
        "env_prepend": {"PATH": ["C:/VC6/Common/MSDev98/Bin", "C:/VC6/VC98/Bin"], "INCLUDE": "C:/VC6/VC98/Include"},
        "flags": ["/O2"], "wrapper": ["wine"], "timeout_seconds": 30})").value();
    auto t = Toolchain::from_json("msvc6", j).value();
    CHECK(t.name == "msvc6");
    CHECK(t.kind == ToolchainKind::msvc);
    CHECK(t.msvc_style());
    CHECK(t.wrapper == std::vector<std::string>{"wine"});
    REQUIRE(t.env_prepend.size() == 2);
    CHECK(t.env_prepend[1].first == "PATH");
    CHECK(t.env_prepend[1].second.find("MSDev98") != std::string::npos);
    CHECK(t.timeout_seconds == 30);
    auto back = Toolchain::from_json("msvc6", t.to_json()).value();
    CHECK(back.to_json() == t.to_json());

    CHECK_FALSE(Toolchain::from_json("x", parse_json(R"({"kind": "msvc"})").value()));
    CHECK_FALSE(Toolchain::from_json("x", parse_json(R"({"kind": "turbo", "compiler": "tc"})").value()));
    CHECK_FALSE(Toolchain::from_json("x", parse_json(R"({"compiler": "cl", "flags": "/O2"})").value()));
}

TEST_CASE("registry load/save with a custom path") {
    auto dir = fs::TempDir::create("decomp-registry").value();
    auto path = dir.path() / "toolchains.json";
    auto reg = ToolchainRegistry::load(path).value();
    Toolchain t;
    t.name = "vs2008";
    t.kind = ToolchainKind::msvc;
    t.compiler = "cl.exe";
    t.flags = {"/O2", "/Gy"};
    reg.upsert(t);
    REQUIRE(reg.save().has_value());
    auto again = ToolchainRegistry::load(path).value();
    REQUIRE(again.find("vs2008"));
    CHECK(again.find("vs2008")->flags == t.flags);
    CHECK(again.remove("vs2008"));
    REQUIRE(fs::write_text(path, "{\"oops\": 1}").has_value());
    CHECK_FALSE(ToolchainRegistry::load(path));
}

TEST_CASE("compiler command lines") {
    Toolchain msvc;
    msvc.kind = ToolchainKind::msvc;
    msvc.compiler = "cl.exe";
    msvc.flags = {"/O2"};
    msvc.include_dirs = {"C:/sdk"};
    msvc.wrapper = {"wine"};
    Compiler c(msvc, "work");
    CompileRequest req;
    req.flags = {"/Gy"};
    req.include_dirs = {"proj/include"};
    auto cmd = c.command_line(req, "work/x.cpp", "work/x.obj");
    CHECK(cmd == std::vector<std::string>{"wine", "cl.exe", "/nologo", "/c", "/O2", "/Gy", "/IC:/sdk", "/Iproj/include", "/Fowork/x.obj", "work/x.cpp"});

    Toolchain gcc;
    gcc.kind = ToolchainKind::gcc;
    gcc.compiler = "g++";
    Compiler g(gcc, "work");
    auto gcmd = g.command_line(CompileRequest{}, "a.cpp", "a.o");
    CHECK(gcmd == std::vector<std::string>{"g++", "-c", "-o", "a.o", "a.cpp"});
}
