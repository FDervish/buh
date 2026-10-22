// SPDX-License-Identifier: GPL-3.0-only

#include "region_permutations.hpp"
#include "../memory.hpp"

namespace Buh {
    extern "C" {
        const void *buh_region_permutations_continue = nullptr;
        void buh_region_permutations_prologue_asm() noexcept;
        void buh_region_permutations_epilogue_asm() noexcept;
        void buh_region_permutations_argument_1_asm() noexcept;
        void buh_region_permutations_argument_2_asm() noexcept;
    }

    void set_up_region_permutation_limit() noexcept {
        // sub esp, 0x4C; push ebx; and eax, 0xFFFF; ...
        auto *prologue = find_signature({ 0x83, 0xEC, 0x4C, 0x53, 0x25, 0xFF, 0xFF, 0x00, 0x00, 0x57, 0x8B, 0xF9, 0x8B, 0x0D, -1, -1, -1, -1, 0x8B, 0x51, 0x34 });
        // ...; pop edi; pop ebx; add esp, 0x4C; ret
        auto *epilogue = find_signature({ 0x8A, 0x44, 0x24, 0x13, 0x5E, 0x5D, 0x5F, 0x5B, 0x83, 0xC4, 0x4C, 0xC3 });
        // The two reads of the function's argument, which moves up the stack with the buffer
        auto *argument_1 = find_signature({ 0x8B, 0x4C, 0x24, 0x58, 0x8D, 0x04, 0x40, 0x8B, 0x44, 0x82, 0x08, 0x8B, 0x91, 0xC4, 0x00, 0x00, 0x00, 0x33, 0xDB });
        auto *argument_2 = find_signature({ 0x8B, 0x4C, 0x24, 0x60, 0x40, 0x0F, 0xBF, 0xD8, 0x89, 0x44, 0x24, 0x18 });

        // All four have to be the same function's, or the stack wouldn't add up
        if(!prologue || !epilogue || !argument_1 || !argument_2 || epilogue - prologue != 0x110 || argument_1 - prologue != 0x15 || argument_2 - prologue != 0xF8) {
            return;
        }
        buh_region_permutations_continue = prologue + 9;
        write_jump(prologue, reinterpret_cast<const void *>(buh_region_permutations_prologue_asm), 9);
        write_jump(epilogue + 6, reinterpret_cast<const void *>(buh_region_permutations_epilogue_asm), 6);
        write_call(argument_1, reinterpret_cast<const void *>(buh_region_permutations_argument_1_asm), 7);
        write_call(argument_2, reinterpret_cast<const void *>(buh_region_permutations_argument_2_asm), 5);
    }
}
