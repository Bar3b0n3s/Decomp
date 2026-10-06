// Decomp test fixture: permute.cpp with independent statements and declarations reordered. Equivalent
// to it, but compiled to other code; the permuter turns it back into byte-exact matches of permute.cpp.
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
    speed *= 0.5f;
    hp -= dmg;
    if (hp < 0) hp = 0;
}

NOINLINE void fill(int* p, int n) {
    for (int i = 0; i < 4; ++i) p[i] = n + i;
}

NOINLINE int two_buffers(int n) {
    int b[4];
    int a[4];
    fill(a, n);
    fill(b, n + 1);
    return a[1] + b[2];
}

int g_a, g_b, g_c;

NOINLINE void set_all(int x) {
    g_c = x - 3;
    g_a = x + 1;
    g_b = x * 2;
}

NOINLINE int weigh(const Player* p, int bonus) {
    int extra = bonus + p->score;
    if (extra > 100) extra = 100;
    int total = p->hp * 4;
    return total + extra;
}

extern "C" void entry() {
    Player p{100, 2.0f, 7};
    p.Hit(5);
    set_all(p.hp);
    ExitProcess(static_cast<unsigned>(two_buffers(g_a) + weigh(&p, g_b) + g_c));
}
