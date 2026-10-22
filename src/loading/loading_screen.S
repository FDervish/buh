;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Instead of Halo's call that draws its loading screen's background: the bitmap, 0 and the color (with the fade in
;# its alpha) on the stack, the rectangle in ecx. Halo's own draws unless buh's did.
.globl _buh_loading_background_asm
_buh_loading_background_asm:
    pushad
    push dword ptr [esp + 0x2C]
    call _buh_loading_background
    add esp, 4
    test al, al
    popad
    jz .Lhalo
    ret
.Lhalo:
    jmp dword ptr [_buh_halo_loading_background]
