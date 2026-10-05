int lib_add(int a, int b);

int lib_mul(int a, int b) {
    int r = 0;
    for (int i = 0; i < b; ++i) r += a ^ i;
    return r;
}

// Two functions with the same code under different names: which is which cannot be told from it.
int lib_twice(int a) { return lib_add(a, a) * 2 + 11; }
int lib_twice_copy(int a) { return lib_add(a, a) * 2 + 11; }
