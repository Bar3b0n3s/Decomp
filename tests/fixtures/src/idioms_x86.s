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
    push 16
    call _switch_biased
    add esp, 4
    push 9
    call _switch_masked
    add esp, 4
    push 1
    call _eh_catcher
    add esp, 4
    .byte 0x68                          # push offset _callbacks (imm32, as MSVC encodes it)
    .long _callbacks
    call _seh_user
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

# A switch whose default cannot happen, on the values 8 to 16: no bounds check, and the first case
# value folded into the byte table's displacement, which so points into the jump table.
    .p2align 4, 0xcc
    .globl _switch_biased
_switch_biased:
    mov eax, dword ptr [esp+4]
    movzx eax, byte ptr [eax + Lbias_index - 8]
    jmp dword ptr [4*eax + Lbias_table]
Lbias_a:
    mov eax, 1
    ret
Lbias_b:
    mov eax, 2
    ret
    .p2align 2, 0xcc
Lbias_table:
    .long Lbias_a, Lbias_b, 0
Lbias_index:
    .byte 0, 2, 2, 2, 2, 2, 2, 2, 1
    .globl _switch_biased_end
_switch_biased_end:

# A switch on a masked value whose default cannot happen: the mask allows 32 values, the byte table
# (which starts with four zeros, as a null jump-table entry would) has the switch's 10.
    .p2align 4, 0xcc
    .globl _switch_masked
_switch_masked:
    mov eax, dword ptr [esp+4]
    and eax, 0x1f
    movzx eax, byte ptr [eax + Lmask_index]
    jmp dword ptr [4*eax + Lmask_table]
Lmask_a:
    mov eax, 1
    ret
Lmask_b:
    mov eax, 2
    ret
Lmask_c:
    mov eax, 3
    ret
    .p2align 2, 0xcc
Lmask_table:
    .long Lmask_a, Lmask_b, Lmask_c
Lmask_index:
    .byte 0, 0, 0, 0, 1, 1, 2, 2, 0, 1
    .globl _switch_masked_end
_switch_masked_end:

# C++ exception handling as MSVC lays it out: the function registers its handler stub; its catch block
# is in the function and returns where to resume; the stub and the unwind code come after the other
# functions (MSVC's .text$x).
    .p2align 4, 0xcc
    .globl _eh_catcher
_eh_catcher:
    push ebp
    mov ebp, esp
    push -1
    .byte 0x68                          # push offset _eh_catcher_stub (imm32, as MSVC encodes it)
    .long _eh_catcher_stub
    mov eax, dword ptr fs:[0]
    push eax
    mov dword ptr fs:[0], esp
    sub esp, 8
    mov dword ptr [ebp-4], 0
    push dword ptr [ebp+8]
    call _release
    add esp, 4
    xor eax, eax
Leh_resume:
    mov ecx, dword ptr [ebp-12]
    mov dword ptr fs:[0], ecx
    mov esp, ebp
    pop ebp
    ret
Leh_catch:
    mov eax, offset Leh_caught
    ret
Leh_caught:
    mov eax, -1
    jmp Leh_resume
    .globl _eh_catcher_end
_eh_catcher_end:

# Structured exception handling as cl.exe writes it with _except_handler3: the scope table lists an
# __except block with its filter and a __finally block, all in the function, which calls the __finally
# block itself one instruction in (past the reload the unwinder needs).
    .p2align 4, 0xcc
    .globl _seh_user
_seh_user:
    push ebp
    mov ebp, esp
    push -1
    .byte 0x68                          # push offset Lseh_scope (imm32, as MSVC encodes it)
    .long Lseh_scope
    .byte 0x68                          # push offset _frame_handler (imm32, as MSVC encodes it)
    .long _frame_handler
    mov eax, dword ptr fs:[0]
    push eax
    mov dword ptr fs:[0], esp
    sub esp, 12
    push ebx
    push esi
    push edi
    mov dword ptr [ebp-24], esp
    mov dword ptr [ebp-4], 1
    mov eax, dword ptr [ebp+8]
    mov ecx, dword ptr [eax]
    mov dword ptr [ebp-28], ecx
    mov dword ptr [ebp-4], 0
    call Lseh_finally_body
    jmp Lseh_done
Lseh_finally:
    mov ecx, dword ptr [ebp-28]
Lseh_finally_body:
    inc ecx
    mov dword ptr [ebp-28], ecx
    ret
Lseh_filter:
    xor eax, eax
    cmp dword ptr [ebp-28], 1
    setg al
    ret
Lseh_except:
    mov esp, dword ptr [ebp-24]
    mov dword ptr [ebp-28], -2
Lseh_done:
    mov dword ptr [ebp-4], -1
    mov eax, dword ptr [ebp-28]
    mov ecx, dword ptr [ebp-16]
    mov dword ptr fs:[0], ecx
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret
    .globl _seh_user_end
_seh_user_end:

    .p2align 4, 0xcc
    .globl _release
_release:
    mov ecx, dword ptr [esp+4]
    dec dword ptr [ecx]
    ret
    .globl _release_end
_release_end:

    .p2align 4, 0xcc
    .globl ___CxxFrameHandler3
___CxxFrameHandler3:
    xor eax, eax
    ret
    .globl ___CxxFrameHandler3_end
___CxxFrameHandler3_end:

    .p2align 4, 0xcc
    .globl _frame_handler
_frame_handler:
    mov eax, 1
    ret
    .globl _frame_handler_end
_frame_handler_end:

# The handler stub starts with two nops, as MSVC writes it; the unwind code jumps to the destructor.
    .p2align 4, 0xcc
    .globl _eh_catcher_stub
_eh_catcher_stub:
    nop
    nop
    mov eax, offset Leh_funcinfo
    jmp ___CxxFrameHandler3
    .globl _eh_catcher_stub_end
_eh_catcher_stub_end:
    .byte 0xcc, 0xcc, 0xcc, 0xcc
    .globl _eh_catcher_unwind
_eh_catcher_unwind:
    lea ecx, [ebp-16]
    jmp _release
    .globl _eh_catcher_unwind_end
_eh_catcher_unwind_end:
    .byte 0xcc, 0xcc, 0xcc, 0xcc

    .section .rdata,"dr"
    .p2align 2
Lseh_scope:
    .long -1, Lseh_filter, Lseh_except
    .long 0, 0, Lseh_finally
Leh_funcinfo:
    .long 0x19930522, 2, Leh_unwind_map, 1, Leh_try_map, 0, 0, 0, 1
Leh_unwind_map:
    .long -1, _eh_catcher_unwind
    .long -1, 0
Leh_try_map:
    .long 0, 0, 1, 1, Leh_handlers
Leh_handlers:
    .long 0x40, 0, 0, Leh_catch

    .data
    .globl _callbacks
_callbacks:
    .long Lilt_callback
