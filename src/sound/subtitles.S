;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Replaces the start of the function that queues a sound permutation, which is in edi
.globl _buh_sound_enqueue_asm
_buh_sound_enqueue_asm:
    pushad
    push edi
    call _buh_sound_enqueued
    add esp, 4
    popad
    movsx eax, cx
    push esi
    lea esi, [eax + eax * 2]
    jmp dword ptr [_buh_sound_enqueue_continue]
