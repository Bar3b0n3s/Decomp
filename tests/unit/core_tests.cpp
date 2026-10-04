#include "core/bytes.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"
#include "core/json.hpp"
#include "core/process.hpp"
#include "core/result.hpp"
#include "core/strings.hpp"

#include <doctest/doctest.h>

using namespace decomp;

namespace {

Result<int> parse_positive(int v) {
    if (v <= 0) return make_error(ErrorCode::invalid_argument, "not positive: {}", v);
    return v;
}

Result<int> doubled(int v) {
    TRY_ASSIGN(int x, parse_positive(v));
    return x * 2;
}

Result<void> check(int v) {
    TRY(parse_positive(v));
    return {};
}

} // namespace

TEST_CASE("TRY and TRY_ASSIGN propagate errors") {
    CHECK(doubled(4).value() == 8);
    auto bad = doubled(-1);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::invalid_argument);
    CHECK(bad.error().message == "not positive: -1");
    CHECK(check(1).has_value());
    CHECK_FALSE(check(0).has_value());
}

TEST_CASE("string helpers") {
    CHECK(parse_u64("0x401000") == 0x401000u);
    CHECK(parse_u64("401000h") == 0x401000u);
    CHECK(parse_u64("1234") == 1234u);
    CHECK_FALSE(parse_u64("0xZZ"));
    CHECK_FALSE(parse_u64(""));
    CHECK(hex(0x401000, 8) == "0x00401000");
    CHECK(hex(255) == "0xff");
    CHECK(trim("  a b \r\n") == "a b");
    CHECK(split("a,b,,c", ',').size() == 4);
    CHECK(split_lines("x\r\ny\n") == std::vector<std::string>{"x", "y"});
    CHECK(replace_all("a.b.c", ".", "::") == "a::b::c");
    CHECK(iequals("PATH", "Path"));
    CHECK(escape_c_string("hi\n\"x\"") == "\"hi\\n\\\"x\\\"\"");
    auto cut = truncate_utf8("hello world", 5);
    CHECK(cut.starts_with("hello\n... [truncated 6 of 11 bytes]"));
}

TEST_CASE("Windows argument quoting follows CommandLineToArgvW rules") {
    CHECK(quote_windows_arg("simple") == "simple");
    CHECK(quote_windows_arg("") == "\"\"");
    CHECK(quote_windows_arg("two words") == "\"two words\"");
    CHECK(quote_windows_arg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(quote_windows_arg("C:\\path with space\\") == "\"C:\\path with space\\\\\"");
    CHECK(quote_windows_arg("C:\\no_space\\") == "C:\\no_space\\");
    std::vector<std::string> argv{"cl.exe", "/c", "/Fo out dir\\x.obj", "a\\\"b"};
    CHECK(build_windows_command_line(argv) == "cl.exe /c \"/Fo out dir\\x.obj\" \"a\\\\\\\"b\"");
}

TEST_CASE("SHA-1 test vectors") {
    CHECK(sha1_hex(std::string_view("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_hex(std::string_view("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    std::string million(1000000, 'a');
    CHECK(sha1_hex(std::string_view(million)) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("ByteReader reads little-endian values with bounds checks") {
    const unsigned char raw[] = {0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 'h', 'i', 0, 0xFF};
    ByteReader r(as_bytes(raw, sizeof(raw)));
    CHECK(r.read<u16>().value() == 0x1234);
    CHECK(r.read<u32>().value() == 0x12345678u);
    CHECK(r.read_cstring().value() == "hi");
    CHECK(r.read<u8>().value() == 0xFF);
    CHECK_FALSE(r.read<u8>());
    CHECK(read_le<u32>(as_bytes(raw, sizeof(raw)), 8) == std::nullopt);
    CHECK(read_cstring_at(as_bytes(raw, sizeof(raw)), 6) == "hi");
}

TEST_CASE("file helpers and TempDir") {
    std::filesystem::path kept;
    {
        auto dir = fs::TempDir::create("decomp-test").value();
        kept = dir.path();
        auto file = dir.path() / "sub" / "a.txt";
        REQUIRE(fs::write_text(file, "hello").has_value());
        CHECK(fs::read_text(file).value() == "hello");
        REQUIRE(fs::append_text(file, " world").has_value());
        CHECK(fs::read_text(file).value() == "hello world");
        CHECK(fs::find_upwards(dir.path() / "sub", "a.txt") == dir.path() / "sub");
        CHECK_FALSE(fs::read_file(dir.path() / "missing.bin"));
    }
    CHECK_FALSE(std::filesystem::exists(kept));
}

TEST_CASE("JSON helpers") {
    auto j = parse_json(R"({"b": 1, "a": "x", "f": true})").value();
    CHECK(dump_compact(j) == R"({"a":"x","b":1,"f":true})");  // keys sorted -> deterministic
    CHECK(json_string(j, "a").value() == "x");
    CHECK_FALSE(json_string(j, "b"));
    CHECK(json_int_or(j, "b", 0) == 1);
    CHECK(json_bool_or(j, "f", false));
    CHECK_FALSE(parse_json("{bad"));
}

#ifndef _WIN32
TEST_CASE("run_process captures output, exit codes, env, cwd and timeouts") {
    ProcessSpec spec;
    spec.argv = {"sh", "-c", "echo out; echo err 1>&2; exit 3"};
    auto r = run_process(spec).value();
    CHECK(r.out == "out\n");
    CHECK(r.err == "err\n");
    CHECK(r.exit_code == 3);
    CHECK_FALSE(r.timed_out);

    spec.argv = {"sh", "-c", "printf %s \"$DECOMP_TEST_VAR\"; pwd"};
    spec.env = {{"DECOMP_TEST_VAR", "value"}};
    spec.cwd = "/";
    r = run_process(spec).value();
    CHECK(r.out == "value/\n");

    ProcessSpec slow;
    slow.argv = {"sh", "-c", "sleep 5"};
    slow.timeout = std::chrono::milliseconds(200);
    r = run_process(slow).value();
    CHECK(r.timed_out);
    CHECK(r.duration < std::chrono::seconds(4));

    ProcessSpec missing;
    missing.argv = {"definitely-not-a-real-program-xyz"};
    CHECK_FALSE(run_process(missing));

    ProcessSpec with_input;
    with_input.argv = {"cat"};
    with_input.stdin_data = "piped";
    CHECK(run_process(with_input).value().out == "piped");
}
#endif
