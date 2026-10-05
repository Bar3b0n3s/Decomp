#pragma once

// The types of tests/fixtures/src/basic.cpp as a project header declares them: compiled and read back,
// they equal the fixture PDB's (tests/unit/types_tests.cpp, and the CI's types check).

struct Player {
    int hp;
    float speed;
    void Hit(int dmg);
    int Score() const;
};
