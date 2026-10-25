;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# The kinds of element, numbered as HudElement::Kind numbers them
.set STATIC, 0
.set METER, 1
.set NUMBER, 2
.set OVERLAY, 3

;# Each of these stands in for the first instructions of one of Halo's element drawing functions: it tells
;# buh_hud_enter what's being drawn, and where the return address is so that the function returns through
;# buh_hud_scope_exit_asm, then runs the instructions the jump here replaced and carries on into the function. After
;# pushad and the two pushes, the return address is at esp + 0x28.

;# hud_draw_bitmap_with_meter, through which every element's bitmap is drawn, has its element in edx
.globl _buh_hud_bitmap_scope_asm
_buh_hud_bitmap_scope_asm:
    pushad
    push OVERLAY
    push edx
    lea eax, [esp + 0x28]
    push eax
    call _buh_hud_enter
    add esp, 0xC
    popad
    sub esp, 0x28
    test bl, bl
    jmp dword ptr [_buh_hud_bitmap_continue]

;# hud_draw_static_element has its element as its third stack argument, at esp + 0x30 after pushad and the kind
.globl _buh_hud_static_scope_asm
_buh_hud_static_scope_asm:
    pushad
    push STATIC
    push dword ptr [esp + 0x30]
    lea eax, [esp + 0x28]
    push eax
    call _buh_hud_enter
    add esp, 0xC
    popad
    sub esp, 0x48
    mov edx, dword ptr [_buh_hud_static_global]
    mov edx, dword ptr [edx]
    jmp dword ptr [_buh_hud_static_continue]

;# hud_draw_numbers has its element as its third stack argument too
.globl _buh_hud_numbers_scope_asm
_buh_hud_numbers_scope_asm:
    pushad
    push NUMBER
    push dword ptr [esp + 0x30]
    lea eax, [esp + 0x28]
    push eax
    call _buh_hud_enter
    add esp, 0xC
    popad
    mov eax, dword ptr [_buh_hud_numbers_global]
    mov eax, dword ptr [eax]
    jmp dword ptr [_buh_hud_numbers_continue]

;# hud_draw_meter's element comes from the bitmap it draws
.globl _buh_hud_meter_scope_asm
_buh_hud_meter_scope_asm:
    pushad
    push METER
    push 0
    lea eax, [esp + 0x28]
    push eax
    call _buh_hud_enter
    add esp, 0xC
    popad
    sub esp, 0xC4
    jmp dword ptr [_buh_hud_meter_continue]

;# Where an element drawing function returns to instead of its caller: leaves its scope, then goes back to the caller
;# with every register and flag as the function left them
.globl _buh_hud_scope_exit_asm
_buh_hud_scope_exit_asm:
    sub esp, 4
    pushad
    pushfd
    call _buh_hud_leave
    mov dword ptr [esp + 0x24], eax
    popfd
    popad
    ret

;# Stands in for the first instruction of the function that draws a quad on the screen, whose one stack argument is
;# the quad's corners
.globl _buh_hud_quad_asm
_buh_hud_quad_asm:
    pushad
    push dword ptr [esp + 0x24]
    call _buh_hud_quad
    add esp, 4
    popad
    sub esp, 0x148
    jmp dword ptr [_buh_hud_quad_continue]
