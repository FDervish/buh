;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

;# Stands in for game_state_data_new(): name and maximum count on the stack, datum size in ebx.
;# The caller cleans up the stack arguments.
.globl _extend_effect_limits_data_new_asm
_extend_effect_limits_data_new_asm:
    push ebx
    push dword ptr [esp + 0x0C]
    push dword ptr [esp + 0x0C]
    call _extend_effect_limits_data_new
    add esp, 0x0C
    ret
