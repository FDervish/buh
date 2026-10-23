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
