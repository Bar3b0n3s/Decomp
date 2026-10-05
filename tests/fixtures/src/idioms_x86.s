# Code layouts that MSVC and VC6 produce and the clang-cl fixtures do not, for function discovery and
# switch tables (tests/unit/discovery_tests.cpp). Every function `f` is followed by a label `f_end`, so
# the map file gives each function's exact bounds. Linked /fixed (no relocations), as VC6 programs are.
    .intel_syntax noprefix
    .text

# An incremental linker's thunk table at the start of the code: five int3 bytes, then a `jmp` per
# function. Calls and function pointers go through the thunks, which are not functions themselves.
    .byte 0xcc, 0xcc, 0xcc, 0xcc, 0xcc
Lilt_switch_one_level:
    .byte 0xe9                          # jmp rel32, as the linker writes it
    .long _switch_one_level - (. + 4)
Lilt_switch_two_level:
    .byte 0xe9
    .long _switch_two_level - (. + 4)
Lilt_callback:
    .byte 0xe9
    .long _callback - (. + 4)
    .byte 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc

    .p2align 4, 0xcc
    .globl _entry
_entry:
    push 3
    call Lilt_switch_one_level
    add esp, 4
    push 3
    call Lilt_switch_two_level
    add esp, 4
    push 16
    call _switch_unchecked
    add esp, 4
    call _calls_exit_helper
    call _tail_caller
    call _dead_code_after_exit
    call dword ptr [_callbacks]
    push 0
    call dword ptr [__imp__ExitProcess@4]
    .globl _entry_end
_entry_end:
    .byte 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc

# A switch as MSVC lays it out: the bounds check, the dispatch, the cases, then the jump table in the
# code section after the function (aligned with a filler instruction).
    .p2align 4, 0xcc
    .globl _switch_one_level
_switch_one_level:
    mov eax, dword ptr [esp+4]
    cmp eax, 4
    ja Lone_default
    jmp dword ptr [4*eax + Lone_table]
Lone_c0:
    mov eax, 10
    ret
Lone_c1:
    mov eax, 11
    ret
Lone_c2:
    mov eax, 12
    ret
Lone_c3:
    mov eax, 13
    ret
Lone_c4:
    mov eax, 14
    ret
Lone_default:
    xor eax, eax
    ret
    .byte 0x8d, 0x49, 0x00                 # lea ecx, [ecx+0]: aligns the table
Lone_table:
    .long Lone_c0, Lone_c1, Lone_c2, Lone_c3, Lone_c4
    .globl _switch_one_level_end
_switch_one_level_end:

# Two-level dispatch: a byte table maps the value to one of a few cases; both tables follow the code.
    .p2align 4, 0x90
    .globl _switch_two_level
_switch_two_level:
    mov eax, dword ptr [esp+4]
    dec eax
    cmp eax, 9
    ja Ltwo_default
    movzx eax, byte ptr [eax + Ltwo_index]
    jmp dword ptr [4*eax + Ltwo_table]
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
    .byte 0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00   # lea esp, [esp+0]
    .p2align 2, 0x90
Ltwo_table:
    .long Ltwo_a, Ltwo_b, Ltwo_c, Ltwo_default
Ltwo_index:
    .byte 0, 1, 2, 1, 0, 3, 3, 2, 1, 0
    .globl _switch_two_level_end
_switch_two_level_end:

# A switch whose default cannot happen (`__assume(0)`), as cl.exe 19.5x lays it out: no bounds check,
# a null entry for the cases that cannot happen, and the byte table right after the jump table.
    .p2align 4, 0xcc
    .globl _switch_unchecked
_switch_unchecked:
    mov eax, dword ptr [esp+4]
    sub eax, 8
    movzx eax, byte ptr [eax + Lunc_index]
    jmp dword ptr [4*eax + Lunc_table]
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
    .long Lunc_a, Lunc_b, Lunc_c, 0
Lunc_index:
    .byte 0, 3, 3, 3, 3, 3, 3, 3, 1, 3, 3, 3, 3, 3, 3, 3, 2
    .globl _switch_unchecked_end
_switch_unchecked_end:

# A helper that never returns (it ends by exiting) and a caller with nothing after the call: the next
# function follows the padding.
    .p2align 4, 0xcc
    .globl _exit_helper
_exit_helper:
    push dword ptr [esp+4]
    call dword ptr [__imp__ExitProcess@4]
    .globl _exit_helper_end
_exit_helper_end:

    .p2align 4, 0xcc
    .globl _calls_exit_helper
_calls_exit_helper:
    push 7
    call _exit_helper
    .globl _calls_exit_helper_end
_calls_exit_helper_end:

# The compiler did not know the helper never returns: the epilogue after the call is the function's.
    .p2align 4, 0xcc
    .globl _dead_code_after_exit
_dead_code_after_exit:
    push esi
    push 9
    call _exit_helper
    add esp, 4
    pop esi
    ret
    .globl _dead_code_after_exit_end
_dead_code_after_exit_end:

# A tail call to a function nothing else calls, placed after int3 padding.
    .p2align 4, 0xcc
    .globl _tail_caller
_tail_caller:
    mov eax, dword ptr [esp+4]
    test eax, eax
    jz Ltail_zero
    jmp _tail_target
Ltail_zero:
    ret
    .globl _tail_caller_end
_tail_caller_end:

    .p2align 4, 0xcc
    .globl _tail_target
_tail_target:
    lea eax, [eax+eax*2]
    ret
    .globl _tail_target_end
_tail_target_end:

# Called only through a pointer in data, by way of its thunk (no relocations: found by scanning the data).
    .p2align 4, 0x90
    .globl _callback
_callback:
    mov eax, 42
    ret
    .globl _callback_end
_callback_end:

# Nothing refers to this one: found in the gap after padding of filler instructions.
    .byte 0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x8d, 0x49, 0x00
    .globl _unreferenced
_unreferenced:
    push ebp
    mov ebp, esp
    mov eax, dword ptr [ebp+8]
    imul eax, eax
    pop ebp
    ret 4
    .globl _unreferenced_end
_unreferenced_end:
    .byte 0xcc, 0xcc, 0xcc, 0xcc

    .data
    .globl _callbacks
_callbacks:
    .long Lilt_callback
