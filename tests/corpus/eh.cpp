// C++ exception handling (compiled with /EHsc) for the bounds corpus: objects destroyed while
// unwinding, try blocks with several handlers, nested try blocks and a rethrow. MSVC keeps x86 catch
// blocks in their function and lists them in the function's EH tables; x64 makes them funclets. Only
// linked and analyzed, never run; eh_rt.c supplies the runtime functions it refers to.

struct Guard {
    int* counter;
    explicit Guard(int* c) : counter(c) { ++*counter; }
    ~Guard() { --*counter; }
};

struct Failure {
    int code;
};

static int g_live;
static int g_caught;

__declspec(noinline) void eh_fail(int code) {
    if (code) throw Failure{code};
}

__declspec(noinline) void eh_throw_int(int n) { throw n; }

// An object with a destructor: unwinding destroys it.
__declspec(noinline) int eh_guarded(int code) {
    Guard g(&g_live);
    eh_fail(code);
    return g_live;
}

// A try block with three handlers.
__declspec(noinline) int eh_catch(int code) {
    try {
        if (code > 100) eh_throw_int(code);
        eh_fail(code);
        return 0;
    } catch (const Failure& f) {
        g_caught += f.code;
        return f.code;
    } catch (int n) {
        return -n;
    } catch (...) {
        return -1;
    }
}

// Nested try blocks with objects in both, and a rethrow.
__declspec(noinline) int eh_nested(int code) {
    Guard outer(&g_live);
    try {
        Guard inner(&g_live);
        try {
            eh_fail(code);
        } catch (const Failure&) {
            ++g_caught;
            throw;
        }
    } catch (...) {
        return g_caught;
    }
    return 0;
}

extern "C" int eh_corpus(int code) { return eh_guarded(code) + eh_catch(code) + eh_nested(code); }
