// SPDX-License-Identifier: GPL-3.0-only

#include "particle_depth_sort.hpp"
#include "extend_effect_limits.hpp"
#include "../memory.hpp"

#include <algorithm>
#include <cstring>

namespace Buh {
    // render_particles' list entry. Signedness matters: it decides where an unset cluster (-1) sorts.
    struct RenderedParticle {
        std::uint16_t particle;
        std::int16_t definition;
        std::int16_t cluster;
        std::uint8_t flag;
        std::uint8_t unused;
    };
    static_assert(sizeof(RenderedParticle) == 8);

    using SortFunction = void (*)(RenderedParticle *, RenderedParticle *, int, int);
    static SortFunction original_sort = nullptr;

    // Read on every sort, since the engine sets it on each map load
    static DatumArray **particle_table = nullptr;

    // The camera plane render_particles measures depth against
    static const float *plane_x = nullptr;
    static const float *plane_y = nullptr;
    static const float *plane_z = nullptr;
    static const float *plane_d = nullptr;

    static float depth_of[PARTICLE_LIMIT];

    extern "C" void particle_depth_sort(RenderedParticle *begin, RenderedParticle *end, int count, int extra) noexcept {
        auto elements = end - begin;
        auto *table = *particle_table;
        if(elements <= 1 || elements > PARTICLE_LIMIT || !table) {
            original_sort(begin, end, count, extra);
            return;
        }

        for(auto *i = begin; i != end; i++) {
            if(i->particle >= PARTICLE_LIMIT) {
                original_sort(begin, end, count, extra);
                return;
            }

            float depth = 0.0F;
            if(i->particle < table->current_size) {
                float position[3];
                std::memcpy(position, table->elements + i->particle * table->element_size + 0x30, sizeof(position));
                depth = position[0] * *plane_x + position[1] * *plane_y + position[2] * *plane_z + *plane_d;

                // A NaN breaks std::sort's strict weak ordering
                if(depth != depth) {
                    depth = 0.0F;
                }
            }
            depth_of[i->particle] = depth;
        }

        // Depth goes last so the engine's batches stay intact; back to front within each one
        std::sort(begin, end, [](const RenderedParticle &a, const RenderedParticle &b) noexcept {
            if(a.definition != b.definition) {
                return a.definition < b.definition;
            }
            if(a.cluster != b.cluster) {
                return a.cluster < b.cluster;
            }
            if(a.flag != b.flag) {
                return a.flag < b.flag;
            }
            return depth_of[a.particle] > depth_of[b.particle];
        });
    }

    void set_up_particle_depth_sort() noexcept {
        auto *table = find_signature({ 0x8B, 0x2D, -1, -1, -1, -1, 0x83, 0xCA, 0xFF, 0x8B, 0xFD, 0xE8, -1, -1, -1, -1, 0x8B, 0xF8, 0x83, 0xFF, 0xFF, 0x0F, 0x84, 0x10, 0x06, 0x00, 0x00 });
        auto *plane = find_signature({ 0xD9, 0x44, 0x24, -1, 0xD8, 0x0D, -1, -1, -1, -1, 0xD9, 0x44, 0x24, -1, 0xD8, 0x0D, -1, -1, -1, -1, 0xDE, 0xC1, 0xD9, 0x44, 0x24, -1, 0xD8, 0x0D, -1, -1, -1, -1, 0xDE, 0xC1, 0xD8, 0x05, -1, -1, -1, -1 });
        auto *call = find_signature({ 0x8B, 0xD0, 0x2B, 0xD1, 0xC1, 0xFA, 0x03, 0x52, 0x50, 0x8B, 0xD1, 0x52, 0x33, 0xF6, 0xE8, -1, -1, -1, -1 });
        if(!table || !plane || !call) {
            return;
        }

        auto read_address = [](const std::byte *at) {
            std::uint32_t address;
            std::memcpy(&address, at, sizeof(address));
            return address;
        };
        particle_table = reinterpret_cast<DatumArray **>(read_address(table + 2));

        // The operands of the dot product render_particles does per particle
        plane_z = reinterpret_cast<const float *>(read_address(plane + 6));
        plane_y = reinterpret_cast<const float *>(read_address(plane + 16));
        plane_x = reinterpret_cast<const float *>(read_address(plane + 28));
        plane_d = reinterpret_cast<const float *>(read_address(plane + 36));

        call += 14;
        std::int32_t relative;
        std::memcpy(&relative, call + 1, sizeof(relative));
        original_sort = reinterpret_cast<SortFunction>(call + 5 + relative);
        retarget_call(call, reinterpret_cast<const void *>(particle_depth_sort));
    }
}
