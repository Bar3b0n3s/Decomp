/* Decomp test fixture: C functions that each toolchain compiles in its own way (calls, a loop, a switch
   with a jump table, arithmetic). Built by different toolchains, the program tells them apart: compiler
   identification (decomp search identify) ranks the one that built it first. */
#define NOINLINE __declspec(noinline)

int g_counter = 3;
int g_table[8] = {1, 1, 2, 3, 5, 8, 13, 21};

NOINLINE int add(int a, int b) { return a + b + g_counter; }

NOINLINE int sum_array(const int* p, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += p[i];
    return s;
}

NOINLINE int classify(int v) {
    switch (v & 7) {
    case 0: return add(v, 1);
    case 1: return v * 3;
    case 2: return sum_array(g_table, v & 7);
    case 3: return v - g_counter;
    case 5: return g_table[v & 7];
    default: return -1;
    }
}

NOINLINE unsigned mix(unsigned x) {
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

void entry(void) {
    volatile int r = add(1, 2) + sum_array(g_table, 8) + classify(g_counter) + (int)mix(7);
    (void)r;
}
