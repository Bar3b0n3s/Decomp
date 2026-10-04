#include "arch/x86/decoder.hpp"
#include "formats/pe.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace decomp;
using namespace decomp::x86;

namespace {

std::optional<Instruction> dec(Arch arch, std::vector<u8> bytes, u64 address) {
    Decoder d(arch);
    return d.decode(as_bytes(bytes.data(), bytes.size()), address);
}

} // namespace

TEST_CASE("x86 decoding: operands, fields and rendering") {
    auto ins = dec(Arch::x86, {0x8B, 0x44, 0x24, 0x08}, 0x401000).value();
    CHECK(ins.length == 4);
    CHECK(render(ins) == "mov eax, dword ptr [esp+0x8]");
    REQUIRE(ins.fields.size() == 1);
    CHECK(ins.fields[0].kind == FieldKind::disp);
    CHECK(ins.fields[0].offset == 3);
    CHECK(ins.fields[0].size == 1);
    CHECK(ins.flow == Flow::none);

    ins = dec(Arch::x86, {0x03, 0x05, 0x00, 0x30, 0x40, 0x00}, 0x401000).value();
    CHECK(render(ins) == "add eax, dword ptr [0x403000]");
    REQUIRE(ins.fields.size() == 1);
    CHECK(ins.fields[0].offset == 2);
    CHECK(ins.fields[0].size == 4);
    CHECK(ins.fields[0].absolute == 0x403000);
    CHECK(ins.memory_target == 0x403000u);
    FieldRenderer sym = [](const Instruction&, const Field& f) -> std::optional<std::string> {
        if (f.absolute == 0x403000) return std::string("g_counter");
        return std::nullopt;
    };
    CHECK(render(ins, sym) == "add eax, dword ptr [g_counter]");

    ins = dec(Arch::x86, {0xFF, 0x24, 0x8D, 0x1C, 0x20, 0x40, 0x00}, 0x4010FE).value();
    CHECK(render(ins) == "jmp dword ptr [ecx*4+0x40201c]");
    CHECK(ins.flow == Flow::indirect_jump);
    CHECK(ins.memory_target == 0x40201Cu);

    ins = dec(Arch::x86, {0x83, 0xEC, 0x08}, 0x401000).value();
    CHECK(render(ins) == "sub esp, 0x8");
    ins = dec(Arch::x86, {0x83, 0xC4, 0xF8}, 0x401000).value();
    CHECK(render(ins) == "add esp, -0x8");
    ins = dec(Arch::x86, {0x8B, 0x45, 0xFC}, 0x401000).value();
    CHECK(render(ins) == "mov eax, dword ptr [ebp-0x4]");

    ins = dec(Arch::x86, {0x68, 0x04, 0x30, 0x40, 0x00}, 0x401000).value();
    CHECK(render(ins) == "push 0x403004");
    REQUIRE(ins.fields.size() == 1);
    CHECK(ins.fields[0].kind == FieldKind::imm);
    CHECK(ins.fields[0].offset == 1);
    CHECK(ins.fields[0].absolute == 0x403004);

    ins = dec(Arch::x86, {0xF3, 0xA5}, 0x401000).value();
    CHECK(render(ins) == "rep movsd");
    ins = dec(Arch::x86, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}, 0x401000).value();
    CHECK(render(ins) == "mov eax, dword ptr fs:[0x0]");
    ins = dec(Arch::x86, {0x8D, 0x04, 0x49}, 0x401000).value();
    CHECK(render(ins) == "lea eax, [ecx+ecx*2]");
}

TEST_CASE("x86 control flow classification and branch targets") {
    auto call = dec(Arch::x86, {0xE8, 0xFB, 0x0F, 0x00, 0x00}, 0x401000).value();
    CHECK(call.flow == Flow::call);
    CHECK(call.branch_target == 0x402000u);
    REQUIRE(call.fields.size() == 1);
    CHECK(call.fields[0].kind == FieldKind::rel);
    CHECK(call.fields[0].offset == 1);
    CHECK(call.fields[0].size == 4);
    CHECK(render(call) == "call 0x402000");

    auto jz = dec(Arch::x86, {0x74, 0x05}, 0x401000).value();
    CHECK(jz.flow == Flow::cond_jump);
    CHECK(jz.branch_target == 0x401007u);
    CHECK(jz.mnemonic == "jz");

    CHECK(dec(Arch::x86, {0xEB, 0xFE}, 0x401000)->flow == Flow::jump);
    CHECK(dec(Arch::x86, {0xC3}, 0x401000)->flow == Flow::ret);
    CHECK(dec(Arch::x86, {0xC2, 0x08, 0x00}, 0x401000)->flow == Flow::ret);
    CHECK(dec(Arch::x86, {0xCC}, 0x401000)->flow == Flow::trap);
    CHECK(dec(Arch::x86, {0xFF, 0xD0}, 0x401000)->flow == Flow::indirect_call);
    CHECK(dec(Arch::x86, {0xFF, 0x15, 0x10, 0x21, 0x40, 0x00}, 0x401000)->memory_target == 0x402110u);
    CHECK(ends_block(Flow::jump));
    CHECK_FALSE(ends_block(Flow::call));
}

TEST_CASE("x64 decoding: RIP-relative operands resolve to absolute targets") {
    auto ins = dec(Arch::x64, {0x8B, 0x05, 0xFA, 0x0F, 0x00, 0x00}, 0x140001000).value();
    REQUIRE(ins.fields.size() == 1);
    CHECK(ins.fields[0].rip_relative);
    CHECK(ins.fields[0].absolute == 0x140002000ull);
    CHECK(ins.memory_target == 0x140002000ull);
    CHECK(render(ins) == "mov eax, dword ptr [0x140002000]");

    ins = dec(Arch::x64, {0x48, 0x83, 0xEC, 0x28}, 0x140001000).value();
    CHECK(render(ins) == "sub rsp, 0x28");
    ins = dec(Arch::x64, {0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00}, 0x140001000).value();
    CHECK(render(ins) == "lea rcx, [0x140001007]");
}

TEST_CASE("invalid bytes decode as (bad) in a sweep") {
    Decoder d(Arch::x86);
    std::vector<u8> bytes = {0x90, 0x0F, 0x0B, 0xC3};
    auto list = d.decode_all(as_bytes(bytes.data(), bytes.size()), 0x1000);
    REQUIRE(list.size() == 3);
    CHECK(list[0].mnemonic == "nop");
    CHECK(list[1].mnemonic == "ud2");
    CHECK(list[1].flow == Flow::trap);
    CHECK(list[2].mnemonic == "ret");

    std::vector<u8> junk = {0xFF, 0xFF};
    auto bad = d.decode_all(as_bytes(junk.data(), junk.size()), 0x2000);
    REQUIRE_FALSE(bad.empty());
    CHECK(bad[0].mnemonic == "(bad)");
}

TEST_CASE("decode a fixture function from the image") {
    auto image = pe::Image::load(test::fixture("x86/basic.exe")).value();
    auto bytes = image.view(0x401060, 15).value();  // add(int, int)
    Decoder d(Arch::x86);
    auto list = d.decode_all(bytes, 0x401060);
    REQUIRE(list.size() == 4);
    CHECK(render(list[0]) == "mov eax, dword ptr [esp+0x8]");
    CHECK(render(list[1]) == "add eax, dword ptr [esp+0x4]");
    CHECK(render(list[2]) == "add eax, dword ptr [0x403000]");
    CHECK(render(list[3]) == "ret");
    CHECK(image.is_relocated(list[2].address + list[2].fields[0].offset));
}
