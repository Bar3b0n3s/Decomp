// Decomp test fixture: a second translation unit (separate PDB module, cross-object references).
#define NOINLINE __declspec(noinline)

extern int g_table[8];
static const char kName[] = "other";

NOINLINE int other_value(int x) { return g_table[x & 7] * 2 + kName[x & 3]; }
