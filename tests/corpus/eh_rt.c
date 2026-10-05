// The runtime functions and data that exception handling refers to (eh.cpp, seh.c). The corpus is only
// linked and analyzed, never run, so they do nothing. The compilers name different ones: MSVC x86
// registers _except_handler4 and __CxxFrameHandler3, clang x86 _except_handler3; on x64 the unwind
// data names __C_specific_handler and __CxxFrameHandler4 (MSVC) or __CxxFrameHandler3 (clang).
#if defined(_WIN64)
typedef unsigned __int64 uintptr_t;
#else
typedef unsigned int uintptr_t;
#endif

uintptr_t __security_cookie = 0xBB40E64E;

// What the compilers' type descriptors point to: the corpus links ??_7type_info@@6B@ to it.
const void* corpus_type_info_vftable[2];

static volatile int g_handled;

__declspec(noreturn) void __stdcall _CxxThrowException(void* object, void* info) {
    g_handled = object != info;
    for (;;) {
    }
}

__declspec(noreturn) void __cdecl __std_terminate(void) {
    for (;;) {
    }
}

int __cdecl __CxxFrameHandler3(void* record, void* frame, void* context, void* dispatcher) {
    return record == frame && context == dispatcher;
}
int __cdecl __CxxFrameHandler4(void* record, void* frame, void* context, void* dispatcher) {
    return record != frame || context != dispatcher;
}
int __cdecl __C_specific_handler(void* record, void* frame, void* context, void* dispatcher) {
    return (record == context) + (frame == dispatcher);
}
int __cdecl _except_handler3(void* record, void* frame, void* context, void* dispatcher) {
    return (record != context) + (frame == dispatcher);
}
int __cdecl _except_handler4(void* record, void* frame, void* context, void* dispatcher) {
    return (record == context) + 2 * (frame != dispatcher);
}
void __cdecl _local_unwind2(void* frame, int level) { g_handled = frame != 0 && level > 0; }
void __cdecl _local_unwind4(void* cookie, void* frame, int level) { g_handled = cookie != frame && level < 0; }
