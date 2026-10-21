// SPDX-License-Identifier: GPL-3.0-only

#include "extend_render_limits.hpp"
#include "../memory.hpp"

#include <cstring>
#include <memory>

// Stock is 1023 vertex blocks, 1023 index blocks and 32768 triangles
#define VERTEX_BLOCK_LIMIT 4096
#define VERTEX_BLOCK_ENTRY_SIZE 16
#define INDEX_BLOCK_ENTRY_SIZE 12
#define INDEX_CAPACITY_LIMIT 0x10000

// Stock is 384 transparent geometry groups
#define SPRITE_BATCH_LIMIT 2048
#define SPRITE_BATCH_ENTRY_SIZE 0xA8

namespace Buh {
    static std::unique_ptr<std::byte []> vertex_block_table;
    static std::unique_ptr<std::byte []> index_block_table;
    static std::unique_ptr<std::byte []> sprite_batch_pending_flags;

    // cmp/test against an immediate. The vertex block table directly follows the array its per-frame reset loop
    // clears, so that loop's end bound is the table's address and must not be moved with it.
    static bool is_comparison_bound(const std::byte *site) noexcept {
        auto byte_at = [site](int back) {
            return *reinterpret_cast<const std::uint8_t *>(site - back);
        };
        return byte_at(1) == 0x3D || byte_at(1) == 0xA9
            || (byte_at(2) == 0x81 && (byte_at(1) & 0xF8) == 0xF8)
            || (byte_at(2) == 0xF7 && (byte_at(1) & 0xF8) == 0xC0);
    }

    // The engine drops the whole batch when either per-frame block table is full
    static void extend_block_limits() noexcept {
        auto *vertex_reserve = find_signature({ 0x57, 0x66, 0x8B, 0xF8, 0x83, 0xC8, 0xFF, 0x85, 0xF6, 0x7E, -1, 0x0F, 0xBF, 0xCF, 0x8D, 0x14, 0x49, 0x8B, 0x0C, 0x95, -1, -1, -1, -1, 0x8D, 0x14, 0x95, -1, -1, -1, -1, 0x53, 0x8B, 0x1A, 0x2B, 0xCE, 0x3B, 0xD9, 0x5B, 0x7D, -1, 0x8B, 0x0D, -1, -1, -1, -1, 0x81, 0xF9, 0xFF, 0x03, 0x00, 0x00, 0x7D, -1 });
        auto *index_reserve = find_signature({ 0x83, 0xC8, 0xFF, 0x85, 0xD2, 0x7E, 0x65, 0xB9, 0x00, 0x80, 0x00, 0x00, 0x56, 0x8B, 0x35, -1, -1, -1, -1, 0x2B, 0xCA, 0x3B, 0xF1, 0x7D, 0x41, 0x8B, 0x0D, -1, -1, -1, -1, 0x81, 0xF9, 0xFF, 0x03, 0x00, 0x00, 0x7D, 0x33, 0x8B, 0xC1, 0x8D, 0x0C, 0x49, 0x89, 0x34, 0x8D, -1, -1, -1, -1 });
        auto *index_buffer_size = find_signature({ 0x68, 0x00, 0x00, 0x03, 0x00, 0x51, 0xFF, 0x52, 0x6C });
        if(!vertex_reserve || !index_reserve || !index_buffer_size) {
            return;
        }

        std::uint32_t vertex_table_address;
        std::memcpy(&vertex_table_address, vertex_reserve + 62, sizeof(vertex_table_address));
        std::vector<std::byte *> vertex_sites;
        for(auto *site : find_value_in_code(vertex_table_address)) {
            if(!is_comparison_bound(site)) {
                vertex_sites.push_back(site);
            }
        }
        if(vertex_sites.size() >= 8 && vertex_sites.size() <= 64) {
            vertex_block_table = std::make_unique<std::byte []>(VERTEX_BLOCK_LIMIT * VERTEX_BLOCK_ENTRY_SIZE);
            auto replacement = reinterpret_cast<std::uint32_t>(vertex_block_table.get());
            for(auto *site : vertex_sites) {
                overwrite(site, replacement);
            }
            overwrite(vertex_reserve + 49, static_cast<std::uint32_t>(VERTEX_BLOCK_LIMIT));
        }

        // The index-buffer lock reads the table's count field (table + 4) by its own absolute address
        std::uint32_t index_table_address;
        std::memcpy(&index_table_address, index_reserve + 0x2F, sizeof(index_table_address));
        auto index_sites = find_value_in_code(index_table_address);
        auto index_field_sites = find_value_in_code(index_table_address + 4);
        if(index_sites.size() == 6 && index_field_sites.size() == 1) {
            index_block_table = std::make_unique<std::byte []>(VERTEX_BLOCK_LIMIT * INDEX_BLOCK_ENTRY_SIZE);
            auto replacement = reinterpret_cast<std::uint32_t>(index_block_table.get());
            for(auto *site : index_sites) {
                overwrite(site, replacement);
            }
            overwrite(index_field_sites[0], replacement + 4);
            overwrite(index_reserve + 0x21, static_cast<std::uint32_t>(VERTEX_BLOCK_LIMIT));

            // The triangle capacity is a separate refusal, and the D3D index buffer (6 bytes per triangle) grows with it
            overwrite(index_reserve + 8, static_cast<std::uint32_t>(INDEX_CAPACITY_LIMIT));
            overwrite(index_buffer_size + 1, static_cast<std::uint32_t>(INDEX_CAPACITY_LIMIT * 6));
        }
    }

    // Past 384 groups a frame, a sprite batch is built and then never drawn. The group array, its sorted index array
    // and the pending flags bit vector all have to grow with the cap.
    static void extend_sprite_batch_limit() noexcept {
        auto *reset = find_signature({ 0x33, 0xC0, 0xB9, 0x0C, 0x00, 0x00, 0x00, 0xBF, -1, -1, -1, -1, 0x89, 0x1D, -1, -1, -1, -1, 0x89, 0x1D, -1, -1, -1, -1, 0x89, 0x1D, -1, -1, -1, -1, 0x89, 0x1D });
        auto *cap_a = find_signature({ 0x8B, 0x0D, -1, -1, -1, -1, 0x33, 0xC0, 0x81, 0xF9, 0x80, 0x01, 0x00, 0x00, 0x7D });
        auto *cap_b = find_signature({ 0x8B, 0x35, -1, -1, -1, -1, 0x81, 0xFE, 0x80, 0x01, 0x00, 0x00, 0xD9, 0x42, 0x04 });
        auto *cap_c = find_signature({ 0x8B, 0x35, -1, -1, -1, -1, 0x81, 0xFE, 0x80, 0x01, 0x00, 0x00, 0x0F, 0x8D });
        auto *alloc = find_signature({ 0x8B, 0x35, -1, -1, -1, -1, 0x68, 0x00, 0xFC, 0x00, 0x00, 0x6A, 0x00, 0xFF, 0xD6 });
        auto *indices_alloc = find_signature({ 0x68, 0x00, 0x03, 0x00, 0x00, 0x6A, 0x00, 0xA3, -1, -1, -1, -1, 0xFF, 0xD6 });
        if(!reset || !cap_a || !cap_b || !cap_c || !alloc || !indices_alloc) {
            return;
        }

        std::uint32_t pending_flags_address;
        std::memcpy(&pending_flags_address, reset + 8, sizeof(pending_flags_address));
        if(pending_flags_address == 0) {
            return;
        }
        auto flag_sites = find_value_in_code(pending_flags_address);
        if(flag_sites.size() != 7) {
            return;
        }

        constexpr std::uint32_t flag_words = (SPRITE_BATCH_LIMIT + 31) / 32;
        sprite_batch_pending_flags = std::make_unique<std::byte []>(flag_words * sizeof(std::uint32_t));
        auto replacement = reinterpret_cast<std::uint32_t>(sprite_batch_pending_flags.get());
        for(auto *site : flag_sites) {
            overwrite(site, replacement);
        }
        overwrite(reset + 3, flag_words);

        overwrite(cap_a + 10, static_cast<std::uint32_t>(SPRITE_BATCH_LIMIT));
        overwrite(cap_b + 8, static_cast<std::uint32_t>(SPRITE_BATCH_LIMIT));
        overwrite(cap_c + 8, static_cast<std::uint32_t>(SPRITE_BATCH_LIMIT));
        overwrite(alloc + 7, static_cast<std::uint32_t>(SPRITE_BATCH_LIMIT * SPRITE_BATCH_ENTRY_SIZE));
        overwrite(indices_alloc + 1, static_cast<std::uint32_t>(SPRITE_BATCH_LIMIT * sizeof(std::uint16_t)));
    }

    void set_up_extend_render_limits() noexcept {
        extend_block_limits();
        extend_sprite_batch_limit();
    }
}
