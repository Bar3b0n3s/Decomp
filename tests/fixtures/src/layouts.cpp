// Types with the layouts a header has to get right, for the type import tests
// (tests/unit/project_types_tests.cpp): compiled and linked at test time with the installed clang-cl, then
// declared from the PDB by `decomp types import`, compiled again and compared with the PDB. Linked
// without a C library, so the few runtime functions the code needs are defined here.
extern "C" int _fltused = 0;
extern "C" int __cdecl _purecall() { return 0; }
void operator delete(void*) noexcept {}
void operator delete(void*, decltype(sizeof(0))) noexcept {}

// Packed tighter than its fields' alignment, and aligned more than its fields'.
#pragma pack(push, 1)
struct Packed {
    char tag;
    int value;
    short tail;
};
#pragma pack(pop)

struct __declspec(align(16)) Aligned {
    float v[3];
};

struct HoldsAligned {
    char c;
    Aligned a;
};

enum Kind : unsigned char { KindNone, KindHero = 4, KindBoss };
enum Flags { FlagA = 1, FlagB = -2 };

struct Node;
typedef void (*Visit)(Node*);

struct Vec2 {
    float x, y;
};

struct Node {
    struct Link {
        Node* next;
        Node* prev;
    };
    Link link;
    Visit visit;
    Kind kind;
    unsigned alive : 1;
    unsigned team : 3;
    unsigned : 4;
    unsigned level : 5;
    short grid[2][3];
    union {
        int raw;
        float scaled;
    };
    struct {
        int a, b;
    } pair;
    Vec2 points[2];
    void (*handlers[2])(int);
    int Node::*field;
    int (Node::*method)(int) const;
    Flags flags;
    static int count;
    int Get(int) const;
    static Node* Make();
};

// Virtual functions: a destructor, a pure one, overloads, other calling conventions.
class Base {
public:
    virtual ~Base();
    virtual int Value() const = 0;
    virtual void Set(int);
    virtual void Set(float);
    int b;
};

class Derived : public Base {
public:
    ~Derived();
    int Value() const;
    virtual void __stdcall Callback(int);
    virtual void Log(const char* format, ...);
    double d;
};

struct Mixin {
    virtual void Mix();
};

// Multiple inheritance, and a virtual base.
class Multi : public Derived, public Mixin {
public:
    char c;
};

class Diamond : public virtual Base {
public:
    int Value() const;
    int v;
};

// A C-style struct with a typedef name.
typedef struct {
    int left, top, right, bottom;
} Rect;

int Node::count = 0;
int Node::Get(int) const { return 0; }
Node* Node::Make() { return nullptr; }
Base::~Base() {}
void Base::Set(int) {}
void Base::Set(float) {}
Derived::~Derived() {}
int Derived::Value() const { return 1; }
void __stdcall Derived::Callback(int) {}
void Derived::Log(const char*, ...) {}
void Mixin::Mix() {}
int Diamond::Value() const { return 2; }

// Parameters and globals make the compiler describe their types. (Globals, not locals initialized with
// {}, which cl.exe may clear with a call to memset.)
__declspec(noinline) int use(Multi& multi, Diamond& diamond) { return multi.c + multi.Value() + diamond.v; }

Packed g_packed;
HoldsAligned g_holds;
Node g_node;
Rect g_rect;

extern "C" int entry() {
    Multi multi;
    Diamond diamond;
    return g_packed.value + g_holds.c + g_node.Get(0) + use(multi, diamond) + diamond.Value() + g_rect.left;
}
