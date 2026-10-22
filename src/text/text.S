;# SPDX-License-Identifier: GPL-3.0-only
;#
;# Derived from Chimera's display_text.

.intel_syntax noprefix

.text

;# buh_draw_text_asm(text, bounds, function): Halo's text functions take the bounds (top, left, bottom, right) in
;# ecx, 0 in eax, and two zeros and the text on the stack
.globl _buh_draw_text_asm
_buh_draw_text_asm:
    pushad
    mov ecx, dword ptr [esp + 0x28]
    xor eax, eax
    push dword ptr [esp + 0x24]
    push 0
    push 0
    call dword ptr [esp + 0x2C + 0xC]
    add esp, 0xC
    popad
    ret
