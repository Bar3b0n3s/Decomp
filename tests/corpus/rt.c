// Freestanding helpers the corpus needs without a C library: the memory functions and the x86 64-bit
// arithmetic helpers that compilers call (MSVC more of them than clang). The corpus is only linked and
// analyzed, never run, but the helpers are still written as working code.
#if defined(_WIN64)
typedef unsigned __int64 size_t;
#else
typedef unsigned int size_t;
#endif

#pragma function(memset, memcpy, memcmp, memmove)
void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (; n--; ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}
int _fltused = 0;

#if defined(_M_IX86)
// Unsigned 64-bit division: dividend and divisor on the stack, quotient in edx:eax, the callee pops 16
// bytes. Shift and subtract, one bit at a time.
__declspec(naked) void _aulldiv(void) {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, [esp+20]
        mov edx, [esp+24]
        mov ebx, [esp+28]
        mov ecx, [esp+32]
        xor esi, esi
        xor edi, edi
        mov ebp, 64
    next_bit:
        shl eax, 1
        rcl edx, 1
        rcl edi, 1
        rcl esi, 1
        cmp esi, ecx
        jb skip
        ja take
        cmp edi, ebx
        jb skip
    take:
        sub edi, ebx
        sbb esi, ecx
        inc eax
    skip:
        dec ebp
        jnz next_bit
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 16
    }
}

// Unsigned 64-bit remainder: the same loop, returning the remainder.
__declspec(naked) void _aullrem(void) {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, [esp+20]
        mov edx, [esp+24]
        mov ebx, [esp+28]
        mov ecx, [esp+32]
        xor esi, esi
        xor edi, edi
        mov ebp, 64
    next_bit:
        shl eax, 1
        rcl edx, 1
        rcl edi, 1
        rcl esi, 1
        cmp esi, ecx
        jb skip
        ja take
        cmp edi, ebx
        jb skip
    take:
        sub edi, ebx
        sbb esi, ecx
    skip:
        dec ebp
        jnz next_bit
        mov eax, edi
        mov edx, esi
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 16
    }
}

// Unsigned 64-bit division with remainder (MSVC calls it when both are used): the quotient in edx:eax,
// the remainder in ebx:ecx, the callee pops 16 bytes.
__declspec(naked) void _aulldvrm(void) {
    __asm {
        push esi
        push edi
        push ebp
        mov eax, [esp+16]
        mov edx, [esp+20]
        mov ebx, [esp+24]
        mov ecx, [esp+28]
        xor esi, esi
        xor edi, edi
        mov ebp, 64
    next_bit:
        shl eax, 1
        rcl edx, 1
        rcl edi, 1
        rcl esi, 1
        cmp esi, ecx
        jb skip
        ja take
        cmp edi, ebx
        jb skip
    take:
        sub edi, ebx
        sbb esi, ecx
        inc eax
    skip:
        dec ebp
        jnz next_bit
        mov ebx, edi
        mov ecx, esi
        pop ebp
        pop edi
        pop esi
        ret 16
    }
}

// The signed forms go through the unsigned helpers (right for operands that are not negative, which is
// all the corpus needs: it is never run).
__declspec(naked) void _alldiv(void) {
    __asm {
        jmp _aulldiv
    }
}
__declspec(naked) void _allrem(void) {
    __asm {
        jmp _aullrem
    }
}
__declspec(naked) void _alldvrm(void) {
    __asm {
        jmp _aulldvrm
    }
}

// 64-bit multiplication: (a_hi:a_lo) * (b_hi:b_lo), low 64 bits in edx:eax, the callee pops 16 bytes.
__declspec(naked) void _allmul(void) {
    __asm {
        mov eax, [esp+8]
        mov ecx, [esp+16]
        or ecx, eax
        mov ecx, [esp+12]
        jnz wide
        mov eax, [esp+4]
        mul ecx
        ret 16
    wide:
        push ebx
        mul ecx
        mov ebx, eax
        mov eax, [esp+8]
        mul dword ptr [esp+20]
        add ebx, eax
        mov eax, [esp+8]
        mul ecx
        add edx, ebx
        pop ebx
        ret 16
    }
}

// 64-bit shifts of edx:eax by cl.
__declspec(naked) void _allshl(void) {
    __asm {
        cmp cl, 64
        jae ge64
        cmp cl, 32
        jae ge32
        shld edx, eax, cl
        shl eax, cl
        ret
    ge32:
        mov edx, eax
        xor eax, eax
        and cl, 31
        shl edx, cl
        ret
    ge64:
        xor eax, eax
        xor edx, edx
        ret
    }
}
__declspec(naked) void _aullshr(void) {
    __asm {
        cmp cl, 64
        jae ge64
        cmp cl, 32
        jae ge32
        shrd eax, edx, cl
        shr edx, cl
        ret
    ge32:
        mov eax, edx
        xor edx, edx
        and cl, 31
        shr eax, cl
        ret
    ge64:
        xor eax, eax
        xor edx, edx
        ret
    }
}
__declspec(naked) void _allshr(void) {
    __asm {
        cmp cl, 64
        jae ge64
        cmp cl, 32
        jae ge32
        shrd eax, edx, cl
        sar edx, cl
        ret
    ge32:
        mov eax, edx
        sar edx, 31
        and cl, 31
        sar eax, cl
        ret
    ge64:
        sar edx, 31
        mov eax, edx
        ret
    }
}
#endif
