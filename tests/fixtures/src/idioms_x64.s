# x64 code layouts for function discovery and switch tables (tests/unit/discovery_tests.cpp): MSVC's
# image-relative switch tables in the code section (one and two levels), leaf functions without unwind
# data, a function whose unwind data is split into a chained entry, and a tail call. Every function `f`
# is followed by a label `f_end`, so the map file gives each function's exact bounds.
    .intel_syntax noprefix
    .text

    .globl entry
    .def entry; .scl 2; .type 32; .endef
    .seh_proc entry
entry:
    sub rsp, 40
    .seh_stackalloc 40
    .seh_endprologue
    mov ecx, 3
    call switch_rva
    mov ecx, 3
    call switch_rva_two_level
    mov ecx, 16
    call switch_rva_unchecked
    mov ecx, 5
    call chained
    mov ecx, 1
    call tail_caller
    xor ecx, ecx
    call qword ptr [rip + __imp_ExitProcess]
    int3
    .seh_endproc
    .globl entry_end
entry_end:

# MSVC's switch: the table holds RVAs and follows the function in the code section. A leaf: no unwind
# data, found by the call.
    .p2align 4, 0xcc
    .globl switch_rva
switch_rva:
    cmp ecx, 4
    ja Lrva_default
    movsxd rax, ecx
    lea rdx, [rip + __ImageBase]
    mov ecx, dword ptr [rdx + 4*rax + Lrva_table@IMGREL]
    add rcx, rdx
    jmp rcx
Lrva_c0:
    mov eax, 10
    ret
Lrva_c1:
    mov eax, 11
    ret
Lrva_c2:
    mov eax, 12
    ret
Lrva_c3:
    mov eax, 13
    ret
Lrva_c4:
    mov eax, 14
    ret
Lrva_default:
    xor eax, eax
    ret
    .p2align 2, 0xcc
Lrva_table:
    .long Lrva_c0@IMGREL, Lrva_c1@IMGREL, Lrva_c2@IMGREL, Lrva_c3@IMGREL, Lrva_c4@IMGREL
    .globl switch_rva_end
switch_rva_end:

# Two-level dispatch through a byte table, both tables after the code.
    .p2align 4, 0xcc
    .globl switch_rva_two_level
switch_rva_two_level:
    dec ecx
    cmp ecx, 9
    ja Ltwo_default
    movsxd rcx, ecx
    lea rdx, [rip + __ImageBase]
    movzx eax, byte ptr [rdx + rcx + Ltwo_index@IMGREL]
    mov ecx, dword ptr [rdx + 4*rax + Ltwo_table@IMGREL]
    add rcx, rdx
    jmp rcx
Ltwo_a:
    mov eax, 1
    ret
Ltwo_b:
    mov eax, 2
    ret
Ltwo_c:
    mov eax, 3
    ret
Ltwo_default:
    or eax, -1
    ret
    .p2align 2, 0xcc
Ltwo_table:
    .long Ltwo_a@IMGREL, Ltwo_b@IMGREL, Ltwo_c@IMGREL, Ltwo_default@IMGREL
Ltwo_index:
    .byte 0, 1, 2, 1, 0, 3, 3, 2, 1, 0
    .globl switch_rva_two_level_end
switch_rva_two_level_end:

# A switch whose default cannot happen (`__assume(0)`): no bounds check, a null entry for the cases that
# cannot happen, the byte table right after the RVA table.
    .p2align 4, 0xcc
    .globl switch_rva_unchecked
switch_rva_unchecked:
    lea eax, [rcx-8]
    movsxd rax, eax
    lea rdx, [rip + __ImageBase]
    movzx eax, byte ptr [rdx + rax + Lunc_index@IMGREL]
    mov ecx, dword ptr [rdx + 4*rax + Lunc_table@IMGREL]
    add rcx, rdx
    jmp rcx
Lunc_a:
    mov eax, 1
    ret
Lunc_b:
    mov eax, 2
    ret
Lunc_c:
    mov eax, 3
    ret
    .p2align 2, 0xcc
Lunc_table:
    .long Lunc_a@IMGREL, Lunc_b@IMGREL, Lunc_c@IMGREL, 0
Lunc_index:
    .byte 0, 3, 3, 3, 3, 3, 3, 3, 1, 3, 3, 3, 3, 3, 3, 3, 2
    .globl switch_rva_unchecked_end
switch_rva_unchecked_end:

# Unwind data in two entries: the second is chained to the first. One function.
    .p2align 4, 0xcc
    .globl chained
    .def chained; .scl 2; .type 32; .endef
    .seh_proc chained
chained:
    push rbx
    .seh_pushreg rbx
    sub rsp, 32
    .seh_stackalloc 32
    .seh_endprologue
    mov ebx, ecx
    call helper
    .seh_startchained
    .seh_endprologue
    add eax, ebx
    add rsp, 32
    pop rbx
    ret
    .seh_endchained
    .seh_endproc
    .globl chained_end
chained_end:

    .p2align 4, 0xcc
    .globl helper
helper:
    lea eax, [rcx+1]
    ret
    .globl helper_end
helper_end:

# A tail call to a leaf nothing else calls, after int3 padding.
    .p2align 4, 0xcc
    .globl tail_caller
tail_caller:
    test ecx, ecx
    jz Ltail_zero
    jmp tail_target
Ltail_zero:
    xor eax, eax
    ret
    .globl tail_caller_end
tail_caller_end:

    .p2align 4, 0xcc
    .globl tail_target
tail_target:
    lea eax, [rcx+rcx*2]
    ret
    .globl tail_target_end
tail_target_end:

# Nothing refers to this leaf: found in the gap after the padding.
    .p2align 4, 0xcc
    .globl unreferenced
unreferenced:
    imul eax, ecx, 7
    ret
    .globl unreferenced_end
unreferenced_end:
    int3
    int3
