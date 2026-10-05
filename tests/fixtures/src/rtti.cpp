// Classes with virtual functions, compiled with /GR (MSVC run-time type information), for the RTTI and
// vtable tests (tests/unit/rtti_tests.cpp): single inheritance in a namespace, a struct, multiple
// inheritance (two vftables) and virtual inheritance. Linked without a C library, so type_info's
// vftable is a stand-in (/alternatename in build_fixtures.sh).
extern "C" const void* rtti_type_info_vftable[2] = {nullptr, nullptr};

namespace game {

class Shape {
public:
    virtual int area() const { return 0; }
    virtual int sides() const { return 0; }
    int id = 1;
};

class Square : public Shape {
public:
    int area() const override { return side * side; }
    int sides() const override { return 4; }
    int side = 3;
};

} // namespace game

struct Named {
    virtual const char* name() const { return "named"; }
};

// Two bases with vftables: a vftable for each.
class Unit : public game::Square, public Named {
public:
    int area() const override { return 9; }
    const char* name() const override { return "unit"; }
};

class Base {
public:
    virtual int value() const { return 1; }
    int b = 0;
};

// A virtual base.
class Middle : public virtual Base {
public:
    int value() const override { return 2; }
};

__declspec(noinline) int use(const game::Shape& s) { return s.area() + s.sides(); }
__declspec(noinline) const char* use_named(const Named& n) { return n.name(); }
__declspec(noinline) int use_base(const Base& b) { return b.value(); }

extern "C" int entry() {
    game::Square square;
    Unit unit;
    Middle middle;
    return use(square) + use(unit) + (use_named(unit) != nullptr) + use_base(middle);
}
