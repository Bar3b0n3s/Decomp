// CodeView type records (formats/codeview.hpp) and type layouts (analysis/types.hpp): the fixture PDBs'
// types, and a header compiled with clang-cl /Z7.

#include "analysis/program.hpp"
#include "analysis/types.hpp"
#include "core/fs.hpp"
#include "core/process.hpp"
#include "formats/coff.hpp"
#include "formats/pdb.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <ostream>  // doctest prints std::string_view with operator<<, which MSVC declares without it
#include <string>
#include <string_view>
#include <vector>

using namespace decomp;

namespace {

TypeCatalog pdb_catalog(const char* fixture) {
    auto reader = pdb::Reader::load(test::fixture(fixture)).value();
    return TypeCatalog::from(reader.types(), 4);
}

// The path of a field reference, "~" marking one inside the field; "-" for none.
std::string path_at(const TypeCatalog& catalog, std::string_view type, u64 offset) {
    const auto ref = catalog.field_ref(type, offset);
    return ref ? ref->path + (ref->exact ? "" : "~") : "-";
}

FieldLayout field(std::string name, u64 offset, u64 size, std::string type) {
    FieldLayout f;
    f.name = std::move(name);
    f.offset = offset;
    f.size = size;
    f.type = std::move(type);
    return f;
}

// The type records of `source` compiled by clang-cl with /Z7 for `arch`; nullopt (with a message) when
// clang-cl is not installed.
std::optional<codeview::TypeStream> compile_types(Arch arch, std::string_view source, const std::filesystem::path& dir) {
    const auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping the compiled type records");
        return std::nullopt;
    }
    const auto cpp = dir / "probe.cpp";
    const auto obj = dir / (arch == Arch::x86 ? "probe32.obj" : "probe64.obj");
    REQUIRE(fs::write_text(cpp, source));
    ProcessSpec cc;
    cc.argv = {tools->clang_cl, arch == Arch::x86 ? "--target=i686-pc-windows-msvc" : "--target=x86_64-pc-windows-msvc",
               "/nologo", "/c", "/Z7", "/clang:-fstandalone-debug", "/Fo" + fs::to_utf8(obj), fs::to_utf8(cpp)};
    const auto run = run_process(cc);
    REQUIRE(run);
    INFO(run->out << run->err);
    REQUIRE(run->ok());
    const auto object = coff::Object::load(obj).value();
    for (const auto& section : object.sections())
        if (section.name == ".debug$T") return codeview::TypeStream::from_debug_t(section.data).value();
    FAIL("no .debug$T section");
    return std::nullopt;
}

constexpr std::string_view kProbe = R"(struct Vec2 { float x, y; };
enum class Kind : unsigned char { None, Hero = 4, Boss };
enum Flags { FlagA = 1, FlagB = -2 };
union Value { int i; float f; char bytes[4]; };
struct Entity {
    void (*callback)(int);
    Vec2 pos;
    Kind kind;
    unsigned alive : 1;
    unsigned team : 3;
    short grid[2][3];
    Entity* next;
    const char* name;
    Value value;
    union { int raw; float scaled; };
    struct { int a, b; } pair;
    Flags flags;
};
Entity* probe_entity;
)";

} // namespace

TEST_CASE("PDB type records: the fixture's Player, and the types of its functions") {
    auto reader = pdb::Reader::load(test::fixture("x86/basic.pdb")).value();
    const auto& types = reader.types();
    REQUIRE(types.end() > types.first());
    const auto catalog = TypeCatalog::from(types, 4);

    const TypeLayout* player = catalog.find("Player");
    REQUIRE(player);
    CHECK(player->kind == TypeKind::structure);
    CHECK(player->size == 8);
    REQUIRE(player->fields.size() == 2);
    CHECK(player->fields[0].name == "hp");
    CHECK(player->fields[0].offset == 0);
    CHECK(player->fields[0].size == 4);
    CHECK(player->fields[0].type == "int");
    CHECK(player->fields[1].name == "speed");
    CHECK(player->fields[1].offset == 4);
    CHECK(player->fields[1].type == "float");
    CHECK(player->field_at(5) == &player->fields[1]);
    CHECK(player->field_at(8) == nullptr);
    CHECK(to_text(*player) == "struct Player  // 8 bytes\n  +0x00  int hp\n  +0x04  float speed\n");
    CHECK(to_json(*player) == parse_json(R"({"name": "Player", "kind": "struct", "size": 8, "fields": [
        {"name": "hp", "offset": 0, "size": 4, "type": "int"}, {"name": "speed", "offset": 4, "size": 4, "type": "float"}]})")
                                  .value());

    CHECK(path_at(catalog, "Player", 0) == "hp");
    CHECK(path_at(catalog, "Player", 4) == "speed");
    CHECK(path_at(catalog, "Player", 6) == "speed~");
    CHECK(path_at(catalog, "Player", 8) == "-");
    CHECK(path_at(catalog, "Enemy", 0) == "-");

    // Procedures name their function types: Player::Hit is a __thiscall member of Player taking an int.
    const auto& procedures = reader.procedures();
    const auto hit = std::ranges::find(procedures, std::string("Player::Hit"), &pdb::Procedure::name);
    REQUIRE(hit != procedures.end());
    const auto function = types.function(hit->type_index);
    REQUIRE(function);
    CHECK(function->calling_convention == 0x0b);
    CHECK(types.udt_name(function->class_type) == "Player");
    CHECK(types.pointee_udt(function->this_type) == "Player");
    CHECK(types.name_of(function->this_type) == "Player* const");
    CHECK(function->parameters == std::vector<codeview::TypeIndex>{0x74});
    const auto sum = std::ranges::find(procedures, std::string("sum_array"), &pdb::Procedure::name);
    REQUIRE(sum != procedures.end());
    CHECK(types.name_of(sum->type_index) == "int (const int*, int)");

    // A program has them by function address; copies with other symbols share them.
    const auto program = Program::open(test::fixture("x86/basic.exe")).value();
    const ProgramTypes& pdb = program.pdb_types();
    CHECK(pdb.catalog.find("Player"));
    const auto hit_type = pdb.function_types.find(*program.resolve("Player::Hit"));
    REQUIRE(hit_type != pdb.function_types.end());
    CHECK(pdb.stream.name_of(hit_type->second) == "void (int)");
    const Program copy = program.with_symbols(program.symbols());
    CHECK(&copy.pdb_types() == &pdb);
    const auto without = Program::open(test::fixture("x86/basic.exe"), OpenOptions{.use_pdb = false}).value();
    CHECK(without.pdb_types().catalog.empty());
    CHECK(without.pdb_types().function_types.empty());
}

TEST_CASE("PDB type records: classes with virtual functions, multiple and virtual inheritance") {
    const auto catalog = pdb_catalog("x86/rtti.pdb");

    const TypeLayout* shape = catalog.find("game::Shape");
    REQUIRE(shape);
    CHECK(shape->kind == TypeKind::class_);
    CHECK(shape->size == 8);
    CHECK(shape->vfptr == 0);
    CHECK(shape->vtable_slots == 2);
    REQUIRE(shape->virtuals.size() == 2);
    CHECK(shape->virtuals[0].name == "area");
    CHECK(shape->virtuals[0].slot == 0);
    CHECK(shape->virtuals[1].name == "sides");
    CHECK(shape->virtuals[1].slot == 1);
    CHECK(to_text(*shape) ==
          "class game::Shape  // 8 bytes, vtable of 2\n  +0x00  vfptr\n  +0x04  int id\n  virtual area  // slot 0\n  virtual sides  // slot 1\n");

    const TypeLayout* square = catalog.find("game::Square");
    REQUIRE(square);
    CHECK(square->size == 12);
    REQUIRE(square->bases.size() == 1);
    CHECK(square->bases[0].name == "game::Shape");
    CHECK(square->bases[0].offset == 0);
    CHECK_FALSE(square->bases[0].is_virtual);
    CHECK_FALSE(square->vfptr);
    CHECK(square->vtable_slots == 2);
    REQUIRE(square->virtuals.size() == 2);
    CHECK_FALSE(square->virtuals[0].slot);  // overrides

    // Two bases with vftables; a base's fields go by their own names.
    const TypeLayout* unit = catalog.find("Unit");
    REQUIRE(unit);
    CHECK(unit->size == 16);
    REQUIRE(unit->bases.size() == 2);
    CHECK(unit->bases[1].name == "Named");
    CHECK(unit->bases[1].offset == 12);
    CHECK(path_at(catalog, "Unit", 0) == "__vfptr");
    CHECK(path_at(catalog, "Unit", 4) == "id");
    CHECK(path_at(catalog, "Unit", 8) == "side");
    CHECK(path_at(catalog, "Unit", 12) == "Named::__vfptr");
    CHECK(path_at(catalog, "Unit", 14) == "Named::__vfptr~");

    // A virtual base: a vbptr, and the base wherever the most derived class puts it.
    const TypeLayout* middle = catalog.find("Middle");
    REQUIRE(middle);
    CHECK(middle->size == 12);
    REQUIRE(middle->bases.size() == 1);
    CHECK(middle->bases[0].is_virtual);
    CHECK(middle->vbptr == 0);
    CHECK_FALSE(middle->vfptr);
    CHECK(path_at(catalog, "Middle", 0) == "__vbptr");
    CHECK(path_at(catalog, "Middle", 4) == "-");
    CHECK(to_text(*middle) == "class Middle : virtual Base  // 12 bytes\n  +0x00  vbptr\n  virtual value  // override\n");
}

TEST_CASE("type records of a header compiled with clang-cl /Z7: bitfields, arrays, unions, enums, nested types") {
    auto dir = fs::TempDir::create("decomp-codeview").value();
    const auto types = compile_types(Arch::x86, kProbe, dir.path());
    if (!types) return;
    const auto catalog = TypeCatalog::from(*types, 4);

    const TypeLayout* entity = catalog.find("Entity");
    REQUIRE(entity);
    CHECK(to_text(*entity) == R"(struct Entity  // 60 bytes
  +0x00  void (__cdecl* callback)(int)
  +0x04  Vec2 pos
  +0x0c  Kind kind
  +0x10  unsigned int alive : 1  // bit 0
  +0x10  unsigned int team : 3  // bit 1
  +0x14  short grid[2][3]
  +0x20  Entity* next
  +0x24  const char* name
  +0x28  Value value
  +0x2c  int raw
  +0x2c  float scaled
  +0x30  Entity::<unnamed-type-pair> pair
  +0x38  Flags flags
)");
    const FieldLayout* grid = entity->field_at(0x14);
    REQUIRE(grid);
    CHECK(grid->dimensions == std::vector<u64>{2, 3});
    CHECK(entity->field_at(0x20)->pointee == "Entity");
    CHECK(entity->field_at(0x04)->udt == "Vec2");

    CHECK(path_at(catalog, "Entity", 0x08) == "pos.y");
    CHECK(path_at(catalog, "Entity", 0x10) == "alive");
    CHECK(path_at(catalog, "Entity", 0x14) == "grid[0][0]");
    CHECK(path_at(catalog, "Entity", 0x1a) == "grid[1][0]");
    CHECK(path_at(catalog, "Entity", 0x1e) == "grid[1][2]");
    CHECK(path_at(catalog, "Entity", 0x29) == "value.i~");
    CHECK(path_at(catalog, "Entity", 0x2c) == "raw");
    CHECK(path_at(catalog, "Entity", 0x34) == "pair.b");
    CHECK(path_at(catalog, "Entity", 0x38) == "flags");

    const TypeLayout* kind = catalog.find("Kind");
    REQUIRE(kind);
    CHECK(kind->kind == TypeKind::enumeration);
    CHECK(kind->size == 1);
    CHECK(to_text(*kind) == "enum Kind : unsigned char  // 1 byte\n  None = 0\n  Hero = 4\n  Boss = 5\n");
    // A negative enumerator reads as one, whatever numeric leaf the compiler wrote it in.
    const TypeLayout* flags = catalog.find("Flags");
    REQUIRE(flags);
    REQUIRE(flags->enumerators.size() == 2);
    CHECK(flags->enumerators[1].value == -2);
    const TypeLayout* value = catalog.find("Value");
    REQUIRE(value);
    CHECK(value->kind == TypeKind::union_);
    CHECK(value->fields.size() == 3);

    // x64: pointers are 8 bytes.
    const auto types64 = compile_types(Arch::x64, kProbe, dir.path());
    REQUIRE(types64);
    const auto catalog64 = TypeCatalog::from(*types64, 8);
    const TypeLayout* entity64 = catalog64.find("Entity");
    REQUIRE(entity64);
    CHECK(entity64->size == 80);
    CHECK(entity64->field_at(0)->size == 8);
    CHECK(path_at(catalog64, "Entity", 0x28) == "next");
    CHECK(entity64->field_at(0x28)->size == 8);
    CHECK_FALSE(compare_layouts(*entity64, *entity).empty());
}

TEST_CASE("comparing layouts names each difference") {
    TypeLayout expected;
    expected.name = "Player";
    expected.size = 8;
    expected.fields = {field("hp", 0, 4, "int"), field("speed", 4, 4, "float")};
    CHECK(compare_layouts(expected, expected).empty());

    TypeLayout actual = expected;
    actual.kind = TypeKind::class_;
    actual.size = 16;
    actual.fields = {field("speed", 8, 8, "double"), field("extra", 4, 4, "int")};
    CHECK(compare_layouts(actual, expected) == std::vector<std::string>{
                                                   "class, expected struct",
                                                   "size 16, expected 8",
                                                   "field hp missing (expected at +0x0)",
                                                   "field speed at +0x8, expected +0x4",
                                                   "field speed is 8 bytes, expected 4",
                                                   "field speed: double, expected float",
                                                   "field extra at +0x4 not expected",
                                               });

    // Tables, virtual methods, bases, bits.
    TypeLayout base = expected;
    base.fields.push_back(field("flags", 8, 4, "unsigned int"));
    base.fields.back().bit_offset = 0;
    base.fields.back().bit_width = 3;
    base.size = 12;
    TypeLayout derived = base;
    derived.bases.push_back({"Base", 0, false});
    derived.vfptr = 0;
    derived.vtable_slots = 2;
    derived.virtuals = {{"Update", 0, false}, {"Draw", 1, true}, {"Tick", std::nullopt, false}};
    derived.fields.back().bit_offset = 2;
    CHECK(compare_layouts(derived, base) == std::vector<std::string>{
                                                "base Base at +0x0 not expected",
                                                "vfptr at +0x0, expected none",
                                                "2 vtable slots, expected 0",
                                                "virtual slot 0: Update, expected none",
                                                "virtual slot 1: Draw = 0, expected none",
                                                "override Tick not expected",
                                                "field flags: 3 bits at bit 2, expected 3 bits at bit 0",
                                            });

    TypeLayout color;
    color.name = "Color";
    color.kind = TypeKind::enumeration;
    color.size = 4;
    color.underlying = "int";
    color.enumerators = {{"Red", 0}, {"Green", 1}};
    TypeLayout other = color;
    other.underlying = "unsigned char";
    other.size = 1;
    other.enumerators = {{"Green", 2}, {"Blue", 3}};
    CHECK(compare_layouts(other, color) == std::vector<std::string>{
                                               "size 1, expected 4",
                                               "underlying type unsigned char, expected int",
                                               "enumerator Red missing (expected 0)",
                                               "enumerator Green = 2, expected 1",
                                               "enumerator Blue = 3 not expected",
                                           });
}

TEST_CASE("field declarations as C++ writes them") {
    CHECK(field_declaration(field("hp", 0, 4, "int")) == "int hp");
    CHECK(field_declaration(field("name", 0, 16, "char[16]")) == "char name[16]");
    CHECK(field_declaration(field("grid", 0, 12, "short[2][3]")) == "short grid[2][3]");
    CHECK(field_declaration(field("cb", 0, 4, "void (__stdcall*)(int, char*)")) == "void (__stdcall* cb)(int, char*)");
    CHECK(field_declaration(field("next", 0, 4, "Entity*")) == "Entity* next");
    auto bits = field("alive", 0, 4, "unsigned int");
    bits.bit_offset = 0;
    bits.bit_width = 1;
    CHECK(field_declaration(bits) == "unsigned int alive : 1");
}

TEST_CASE("CodeView records that do not decode leave the rest readable") {
    // A truncated stream is an error; a record of an unknown kind is only skipped by layout readers.
    std::vector<std::byte> truncated = {std::byte{0x10}, std::byte{0x00}, std::byte{0x05}, std::byte{0x15}};
    CHECK_FALSE(codeview::TypeStream::parse(truncated));
    std::vector<std::byte> unknown = {std::byte{0x06}, std::byte{0x00}, std::byte{0x34}, std::byte{0x12},
                                      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto stream = codeview::TypeStream::parse(unknown).value();
    CHECK(stream.end() == codeview::kFirstTypeIndex + 1);
    CHECK_FALSE(stream.udt(codeview::kFirstTypeIndex));
    CHECK_FALSE(layout_of(stream, codeview::kFirstTypeIndex, 4));
    CHECK(TypeCatalog::from(stream, 4).empty());
    CHECK(stream.name_of(0x74) == "int");
    CHECK(stream.name_of(0x0470) == "char*");
    CHECK(stream.size_of(0x0670, 8) == 8);
    CHECK(stream.name_of(0x5000) == "<type 0x5000>");
    CHECK_FALSE(codeview::TypeStream::from_debug_t(std::vector<std::byte>{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}));
}
