// A program linked with minilib.lib (src/minilib) for the library-matching tests.
int lib_mul(int a, int b);
int lib_twice(int a);
int lib_twice_copy(int a);
unsigned lib_strlen(const char* s);
int lib_count_char(const char* s, char c);

static int own(int x) { return x * 5 - 2; }

int entry(void) {
    return lib_twice(3) + lib_twice_copy(4) + lib_mul(2, 5) + (int)lib_strlen("abc") + lib_count_char("banana", 'a') + own(7);
}
