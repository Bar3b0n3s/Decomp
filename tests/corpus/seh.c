// Structured exception handling (__try/__except/__finally, __leave) for the bounds corpus. MSVC puts
// the filter, the handler and the finally block of each __try in the function (x86, listed in the
// function's scope table) or makes funclets of them (x64, listed in the unwind data). Only linked and
// analyzed, never run; eh_rt.c supplies the handler functions.

unsigned long __cdecl _exception_code(void);

volatile int g_seh_state;

static int seh_is_access_violation(unsigned long code) { return code == 0xC0000005ul; }

__declspec(noinline) int seh_except(const int* p) {
    __try {
        return *p;
    } __except (seh_is_access_violation(_exception_code())) {
        return -1;
    }
}

__declspec(noinline) int seh_finally(const int* p) {
    int r = 0;
    __try {
        r = *p;
        if (r < 0) __leave;
        r += g_seh_state;
    } __finally {
        g_seh_state = r;
    }
    return r;
}

__declspec(noinline) int seh_nested(const int* p, const int* q) {
    int r = 0;
    __try {
        __try {
            r = *p / *q;
        } __finally {
            ++g_seh_state;
        }
    } __except (g_seh_state > 1) {
        r = -2;
    }
    return r;
}

int seh_corpus(const int* p, const int* q) { return seh_except(p) + seh_finally(p) + seh_nested(p, q); }
