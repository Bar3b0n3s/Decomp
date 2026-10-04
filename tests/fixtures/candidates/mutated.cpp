// Decomp test fixture: candidate sources that deliberately differ from src/basic.cpp, one kind of
// difference per function, so the diff engine's classifications can be asserted.
#define NOINLINE __declspec(noinline)

int g_counter = 3;
int g_table[8] = {1, 1, 2, 3, 5, 8, 13, 21};
int g_counter2 = 4;
static int s_calls = 0;

int other_value(int x);

struct Player {
    int hp;
    float speed;
    NOINLINE void Hit(int dmg);
    NOINLINE int Score() const;
};

// Immediate operands differ (clamp to 1 instead of 0).
void Player::Hit(int dmg) {
    hp -= dmg;
    if (hp < 1) hp = 1;
    speed *= 0.5f;
}

// Identical to the original.
int Player::Score() const { return hp * 10 + static_cast<int>(speed); }

// Opcode differs (sub instead of add).
NOINLINE int add(int a, int b) { return a - b + g_counter; }

// Same instruction, different global symbol.
NOINLINE int read_counter() { return g_counter2; }

// Different code shape (extra multiply in the loop).
NOINLINE int sum_array(const int* p, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += p[i] * 2;
    return s;
}

NOINLINE static int helper(int x) {
    ++s_calls;
    return x * 3 + 1;
}

NOINLINE int dispatch(int op, int v) {
    switch (op) {
    case 0: return add(v, 1);
    case 1: return helper(v);
    case 2: return other_value(v);
    case 3: return sum_array(g_table, v & 7);
    case 4: return v - g_counter;
    case 5: return g_table[v & 7] + helper(v + 1);
    default: return -1;
    }
}

// Same instruction, different string literal content.
NOINLINE const char* message() { return "hello there"; }

// Same instructions, different float constant.
NOINLINE float scale(float x) { return x * 2.5f + 0.25f; }

NOINLINE double mix(double a, double b) { return a * 0.75 + b * 0.25; }
