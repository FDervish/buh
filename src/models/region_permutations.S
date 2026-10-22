;# SPDX-License-Identifier: GPL-3.0-only
;#
;# Derived from Balltze's extended region permutation limit.

.intel_syntax noprefix

;# The buffer is the last 0x40 of the function's 0x4C bytes of locals, 2 bytes per permutation, so more locals
;# make it longer and move only the argument
.set EXTRA_LOCALS, (256 - 32) * 2
.set LOCALS, 0x4C + EXTRA_LOCALS

.text

;# Replaces sub esp, 0x4C; push ebx; and eax, 0xFFFF
.globl _buh_region_permutations_prologue_asm
_buh_region_permutations_prologue_asm:
    sub esp, LOCALS
    push ebx
    and eax, 0xFFFF
    jmp dword ptr [_buh_region_permutations_continue]

;# Replaces pop edi; pop ebx; add esp, 0x4C; ret
.globl _buh_region_permutations_epilogue_asm
_buh_region_permutations_epilogue_asm:
    pop edi
    pop ebx
    add esp, LOCALS
    ret

;# Replaces mov ecx, [esp + 0x58]; lea eax, [eax + eax * 2], called, so the argument is 4 bytes further away
.globl _buh_region_permutations_argument_1_asm
_buh_region_permutations_argument_1_asm:
    mov ecx, dword ptr [esp + 0x58 + EXTRA_LOCALS + 4]
    lea eax, [eax + eax * 2]
    ret

;# Replaces mov ecx, [esp + 0x60]; inc eax
.globl _buh_region_permutations_argument_2_asm
_buh_region_permutations_argument_2_asm:
    mov ecx, dword ptr [esp + 0x60 + EXTRA_LOCALS + 4]
    inc eax
    ret
