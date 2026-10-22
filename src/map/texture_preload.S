;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# buh_load_bitmap_texture(bitmap data): load it now, requesting it if it isn't cached and waiting for it
.globl _buh_load_bitmap_texture
_buh_load_bitmap_texture:
    mov eax, dword ptr [esp + 4]
    push 1
    push 1
    call dword ptr [_buh_load_bitmap_function]
    add esp, 8
    ret
