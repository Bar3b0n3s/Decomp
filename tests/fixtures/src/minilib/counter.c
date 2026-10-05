// A static library for the library-matching tests (tests/unit/library_tests.cpp): built into
// minilib.lib, which libuser.c links. Each file is a member of its own.
int lib_counter = 3;

int lib_add(int a, int b) {
    int r = (a + b) * 7 + lib_counter;
    return r - (a >> 2) + (b ^ 0x55);
}
