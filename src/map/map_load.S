;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Stands in for the calls to the map loading function, which takes the map's name in eax and returns in al. The
;# load itself goes through buh_run_map_load, whose result goes back in the eax popad restores.
.globl _buh_load_map_asm
_buh_load_map_asm:
    pushad
    push eax
    call _buh_before_map_load
    add esp, 4
    popad
    pushad
    push eax
    call _buh_run_map_load
    add esp, 4
    mov dword ptr [esp + 0x1C], eax
    popad
    pushad
    call _buh_after_map_load
    popad
    ret

;# buh_call_load_map(map): Halo's map loading function as a C function, on whatever thread calls it
.globl _buh_call_load_map
_buh_call_load_map:
    mov eax, dword ptr [esp + 4]
    push ebx
    push esi
    push edi
    push ebp
    call dword ptr [_buh_load_map]
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

;# Stands in for the calls to the map data reading function, which takes the file offset in edx, the size in ebx and
;# the buffer as its third argument. A null buffer means its allocation failed.
.globl _buh_read_map_data_asm
_buh_read_map_data_asm:
    cmp dword ptr [esp + 0xC], 0
    je .Lout_of_memory
    cmp edx, 0x80000000
    jae .Lvirtual
    jmp dword ptr [_buh_read_map_data]

;# Offsets past the end of any map: fill the buffer here, then let Halo's read finish as usual on nothing
.Lvirtual:
    pushad
    push ebx
    push edx
    push dword ptr [esp + 0x34]
    call _buh_read_virtual_data
    add esp, 0xC
    popad
    xor ebx, ebx
    xor edx, edx
    jmp dword ptr [_buh_read_map_data]

.Lout_of_memory:
    call _buh_map_data_out_of_memory
