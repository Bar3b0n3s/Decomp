#pragma once

// The classes of tests/fixtures/src/rtti.cpp as a project header declares them: compiled and read back,
// they equal the fixture PDB's (tests/unit/types_tests.cpp, and the CI's types check).

namespace game {

class Shape {
public:
    virtual int area() const;
    virtual int sides() const;
    int id;
};

class Square : public Shape {
public:
    int area() const;
    int sides() const;
    int side;
};

} // namespace game

struct Named {
    virtual const char* name() const;
};

// Two bases with vftables.
class Unit : public game::Square, public Named {
public:
    int area() const;
    const char* name() const;
};

class Base {
public:
    virtual int value() const;
    int b;
};

// A virtual base.
class Middle : public virtual Base {
public:
    int value() const;
};
