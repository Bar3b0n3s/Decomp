// Decomp test fixture: functions whose code depends on the order of independent statements and
// declarations. permute_perturbed.cpp has them reordered; the permuter (tests/unit/permute_tests.cpp)
// turns it back into byte-exact matches of this program.
#define NOINLINE __declspec(noinline)

extern "C" int _fltused = 0;  // the MSVC ABI requires this symbol when floating point is used without a CRT
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int code);

struct Player {
    int hp;
    float speed;
    int score;
    NOINLINE void Hit(int dmg);
};

void Player::Hit(int dmg) {
    hp -= dmg;
    if (hp < 0) hp = 0;
    speed *= 0.5f;
}

NOINLINE void fill(int* p, int n) {
    for (int i = 0; i < 4; ++i) p[i] = n + i;
}

NOINLINE int two_buffers(int n) {
    int a[4];
    int b[4];
    fill(a, n);
    fill(b, n + 1);
    return a[1] + b[2];
}

int g_a, g_b, g_c;

NOINLINE void set_all(int x) {
    g_a = x + 1;
    g_b = x * 2;
    g_c = x - 3;
}

NOINLINE int weigh(const Player* p, int bonus) {
    int total = p->hp * 4;
    int extra = bonus + p->score;
    if (extra > 100) extra = 100;
    return total + extra;
}

extern "C" void entry() {
    Player p{100, 2.0f, 7};
    p.Hit(5);
    set_all(p.hp);
    ExitProcess(static_cast<unsigned>(two_buffers(g_a) + weigh(&p, g_b) + g_c));
}
