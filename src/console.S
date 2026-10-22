;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.data
print_format: .string "%s"

.text

;# Halo's console print takes a pointer to an ARGB color in eax, then a format and its arguments
.globl _buh_console_print_asm
_buh_console_print_asm:
    pushad
    mov eax, dword ptr [esp + 0x24]
    push dword ptr [esp + 0x28]
    push offset print_format
    call dword ptr [_buh_console_print_function]
    add esp, 8
    popad
    ret

;# Replaces the first 8 bytes of Halo's console executor. buh's commands return true straight away; anything
;# else replays the two displaced instructions and carries on into Halo's executor.
.globl _buh_console_execute_asm
_buh_console_execute_asm:
    pushad
    push edi
    call _buh_console_line
    add esp, 4
    test al, al
    popad
    jz .Lnot_ours
    mov al, 1
    ret

.Lnot_ours:
    mov al, byte ptr [edi]
    sub esp, 0x500
    jmp dword ptr [_buh_console_execute_continue]

;# Replaces sub esp, 0x80C; push ebx at the start of Halo's script evaluator, which Chimera's Lua execute_script
;# calls directly. It takes the text as its only argument and returns whether it worked in al.
.globl _buh_script_evaluate_asm
_buh_script_evaluate_asm:
    pushad
    push dword ptr [esp + 0x24]
    call _buh_console_execute
    add esp, 4
    test al, al
    popad
    jz .Lnot_a_command
    mov al, 1
    ret

.Lnot_a_command:
    sub esp, 0x80C
    push ebx
    jmp dword ptr [_buh_script_evaluate_continue]

;# Replaces movsx edx, word ptr [count] in Halo's tab completion, between its searches and sorting what they found.
;# Which searches it ran is its first argument, at esp + 0x14 there.
.globl _buh_tab_completion_asm
_buh_tab_completion_asm:
    pushad
    push dword ptr [esp + 0x34]
    call _buh_tab_completion
    add esp, 4
    popad
    mov edx, dword ptr [_buh_tab_completion_count]
    movsx edx, word ptr [edx]
    jmp dword ptr [_buh_tab_completion_continue]
