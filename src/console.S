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
    call _buh_console_execute
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
