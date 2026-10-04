// Decomp test fixture: small freestanding functions covering common code shapes.
// Built by tests/fixtures/build_fixtures.sh with clang-cl + lld-link (no CRT, deterministic /Brepro).
#define NOINLINE __declspec(noinline)

extern "C" int _fltused = 0;  // the MSVC ABI requires this symbol when floating point is used without a CRT

extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int code);

int g_counter = 3;
int g_table[8] = {1, 1, 2, 3, 5, 8, 13, 21};
static int s_calls = 0;

int other_value(int x);  // other.cpp

struct Player {
    int hp;
    float speed;
    NOINLINE void Hit(int dmg);
    NOINLINE int Score() const;
};

void Player::Hit(int dmg) {
    hp -= dmg;
    if (hp < 0) hp = 0;
    speed *= 0.5f;
}

int Player::Score() const { return hp * 10 + static_cast<int>(speed); }

NOINLINE int add(int a, int b) { return a + b + g_counter; }

NOINLINE int read_counter() { return g_counter; }

NOINLINE int sum_array(const int* p, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += p[i];
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

NOINLINE const char* message() { return "hello world"; }

NOINLINE float scale(float x) { return x * 1.5f + 0.25f; }

NOINLINE double mix(double a, double b) { return a * 0.75 + b * 0.25; }

extern "C" __declspec(dllexport) int exported_api(int x) { return dispatch(x & 3, x) + 7; }

extern "C" void entry() {
    Player p{100, 2.0f};
    p.Hit(5);
    int total = add(1, 2) + read_counter() + sum_array(g_table, 8) + dispatch(g_counter, 4) + message()[0] +
                p.Score() + static_cast<int>(scale(2.0f) + mix(1.0, 3.0)) + exported_api(5);
    ExitProcess(static_cast<unsigned>(total));
}
