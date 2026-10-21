// SPDX-License-Identifier: GPL-3.0-only

#include "game_state.hpp"
#include "memory.hpp"

#include <cstring>

// Fixed, so a saved game's pointers are valid on the next launch too. Right below Halo's own block at 0x40000000,
// which Chimera grows to 0x44440000 for 64 MiB of tag data.
#define GAME_STATE_ADDRESS (0x40000000 - GAME_STATE_SIZE)

// Halo keeps its checkpoint and saved game files this much bigger than the game state
#define GAME_STATE_FILE_SLACK 0x40000

namespace Buh {
    // Every 32-bit immediate equal to value whose instruction's last opcode byte is one of opcodes
    template<std::size_t count> static std::vector<std::byte *> find_immediates(std::uint32_t value, const std::uint8_t (&opcodes)[count]) noexcept {
        std::vector<std::byte *> sites;
        for(auto *site : find_value_in_code(value)) {
            auto opcode = *reinterpret_cast<const std::uint8_t *>(site - 1);
            if(std::find(opcodes, opcodes + count, opcode) != opcodes + count) {
                sites.push_back(site);
            }
        }
        return sites;
    }

    bool set_up_extend_game_state() noexcept {
        // game_state_initialize() asks for 0x400000 + 0x40000 bytes at the physical memory map's cursor
        auto *base_load = find_signature({ 0xA1, -1, -1, -1, -1, 0x56, 0x8B, 0xF0, 0xA3, -1, -1, -1, -1, 0x8B, 0x44, 0x24, 0x08, 0x03, 0xC1, 0x50, 0x6A, 0x00, 0xC6, 0x05, -1, -1, -1, -1, 0x01, 0xA3, -1, -1, -1, -1, 0xFF, 0x15 });
        auto *size_push = find_signature({ 0x68, 0x00, 0x00, 0x40, 0x00, 0xB9, 0x00, 0x00, 0x04, 0x00, 0xC7, 0x05, -1, -1, -1, -1, 0xFF, 0xFF, 0xFF, 0xFF, 0xE8 });
        if(!base_load || !size_push) {
            return false;
        }

        // Checkpoints copy the size game_state_initialize() was given, but saved games, cores and the saved game
        // checksum use it as an immediate (push, mov eax/ecx/edi), as do the file size checks (push, cmp eax)
        static const std::uint8_t size_opcodes[] = { 0x68, 0xB8, 0xB9, 0xBF };
        static const std::uint8_t file_size_opcodes[] = { 0x68, 0x3D };
        auto size_sites = find_immediates(GAME_STATE_STOCK_SIZE, size_opcodes);
        auto file_size_sites = find_immediates(GAME_STATE_STOCK_SIZE + GAME_STATE_FILE_SLACK, file_size_opcodes);
        if(size_sites.size() != 7 || file_size_sites.size() != 3) {
            return false;
        }

        auto *block = VirtualAlloc(reinterpret_cast<void *>(GAME_STATE_ADDRESS), GAME_STATE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if(block != reinterpret_cast<void *>(GAME_STATE_ADDRESS)) {
            if(block) {
                VirtualFree(block, 0, MEM_RELEASE);
            }
            return false;
        }

        // mov eax, [physical memory cursor] -> mov eax, GAME_STATE_ADDRESS
        std::uint8_t mov[5] = { 0xB8 };
        std::uint32_t address = GAME_STATE_ADDRESS;
        std::memcpy(mov + 1, &address, sizeof(address));
        overwrite(base_load, mov, sizeof(mov));
        overwrite(size_push + 1, static_cast<std::uint32_t>(GAME_STATE_SIZE - 0x40000));

        for(auto *site : size_sites) {
            overwrite(site, static_cast<std::uint32_t>(GAME_STATE_SIZE));
        }
        for(auto *site : file_size_sites) {
            overwrite(site, static_cast<std::uint32_t>(GAME_STATE_SIZE + GAME_STATE_FILE_SLACK));
        }
        return true;
    }
}
