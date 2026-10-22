;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Stands in for render_window's call to interface_draw_screen(): the HUD draws first, then buh's things
.globl _buh_interface_draw_asm
_buh_interface_draw_asm:
    call dword ptr [_buh_interface_draw]
    pushad
    call _buh_after_interface_draw
    popad
    ret
