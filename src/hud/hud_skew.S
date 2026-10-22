;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Stands in for the HUD bitmap function's call to draw its quad, whose only stack argument is the quad's corners
.globl _buh_hud_bitmap_draw_asm
_buh_hud_bitmap_draw_asm:
    pushad
    push dword ptr [esp + 0x24]
    call _buh_skew_hud_quad
    add esp, 4
    popad
    jmp dword ptr [_buh_hud_bitmap_draw]

;# Replaces sub esp, 0xA8; push ebx; push ebp at the start of the navpoint drawing function, which takes four
;# arguments on the stack, so navpoints (which are drawn as HUD bitmaps) can be told apart
.globl _buh_navpoint_draw_asm
_buh_navpoint_draw_asm:
    mov byte ptr [_buh_drawing_navpoint], 1
    push dword ptr [esp + 0x10]
    push dword ptr [esp + 0x10]
    push dword ptr [esp + 0x10]
    push dword ptr [esp + 0x10]
    call .Lnavpoint_draw
    add esp, 0x10
    mov byte ptr [_buh_drawing_navpoint], 0
    ret

.Lnavpoint_draw:
    sub esp, 0xA8
    push ebx
    push ebp
    jmp dword ptr [_buh_navpoint_draw_continue]
