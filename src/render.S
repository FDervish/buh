;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Stands in for render_window's call to interface_draw_screen(): buh's things in the world, then the HUD, then buh's
;# things on the HUD
.globl _buh_interface_draw_asm
_buh_interface_draw_asm:
    pushad
    call _buh_before_interface_draw
    popad
    call dword ptr [_buh_interface_draw]
    pushad
    call _buh_after_interface_draw
    popad
    ret

;# Stands in for render_window's call to start the transparent geometry: buh's things on the opaque world first. The
;# call goes on with the stack and registers as Halo left them.
.globl _buh_transparent_geometry_asm
_buh_transparent_geometry_asm:
    pushad
    call _buh_before_transparent_geometry
    popad
    jmp dword ptr [_buh_transparent_geometry]
