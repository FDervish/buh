;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Replaces mov eax, [edi]; mov [tag array], eax right after Halo reads the map's tag data, with edi pointing at its
;# header: the packs go in first, so Halo caches the tag array with them in it
.globl _buh_tag_data_loaded_asm
_buh_tag_data_loaded_asm:
    pushad
    push edi
    call _buh_tag_data_loaded
    add esp, 4
    popad
    mov eax, dword ptr [edi]
    push ecx
    mov ecx, dword ptr [_buh_cached_tag_array]
    mov dword ptr [ecx], eax
    pop ecx
    jmp dword ptr [_buh_tag_data_loaded_continue]
