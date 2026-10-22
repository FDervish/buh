// SPDX-License-Identifier: GPL-3.0-only

#include "tag_packs.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../map/map_load.hpp"
#include "../memory.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#define PACK_VERSION 1
#define MAXIMUM_TAGS 0xFFFF
#define MAXIMUM_MOUNTED 64

// Tag space as Halo allocates it; Chimera grows it to 64 MiB
#define TAG_DATA_ADDRESS 0x40440000
#define STOCK_TAG_SPACE_END 0x41B40000

#define SCENARIO_STRUCTURE_BSPS_OFFSET 0x5A4
#define SCENARIO_BSP_SIZE 0x20

namespace Buh {
    // The file tools/tagpack.py writes
    struct PackHeader {
        char magic[4];
        std::uint32_t version;
        std::uint32_t tag_count;
        std::uint32_t tag_data_size;
        std::uint32_t pointer_count;
        std::uint32_t tag_id_count;
        std::uint32_t file_offset_count;
        std::uint32_t model_vertex_count;
        std::uint32_t model_index_count;
        std::uint32_t raw_size;
        std::uint32_t vertex_size;
        std::uint32_t index_size;
        std::uint32_t tags_offset;
        std::uint32_t tag_data_offset;
        std::uint32_t relocations_offset;
        std::uint32_t raw_offset;
        std::uint32_t model_offset;
        char name[32];
    };
    static_assert(sizeof(PackHeader) == 100);

    struct PackTag {
        std::uint32_t classes[3];
        std::uint32_t path;
        std::uint32_t data;
    };
    static_assert(sizeof(PackTag) == 0x14);

    struct TagIDRelocation {
        std::uint32_t offset;
        std::uint32_t tag;
    };

    struct Pack {
        std::string file;
        std::string name;
        HANDLE handle;
        PackHeader header;
        std::vector<PackTag> tags;
        std::vector<std::byte> tag_data;
        std::vector<std::uint32_t> pointers;
        std::vector<TagIDRelocation> tag_ids;
        std::vector<std::uint32_t> file_offsets;
        std::vector<std::uint32_t> model_vertices;
        std::vector<std::uint32_t> model_indices;
        std::vector<std::byte> model;
        bool in_map = false;
    };

    struct TagDataHeader {
        Tag *tag_array;
        std::uint32_t scenario_tag;
        std::uint32_t checksum;
        std::uint32_t tag_count;
        std::uint32_t model_part_count;
        std::uint32_t model_data_file_offset;
        std::uint32_t model_part_count_2;
        std::uint32_t vertex_size;
        std::uint32_t model_data_size;
    };

    // A pack's raw data (bitmaps and sounds), as Halo's file thread sees it
    struct MountedRawData {
        std::uint32_t offset;
        std::uint32_t size;
        HANDLE file;
        std::uint32_t file_offset;
    };

    // Never freed: Halo's file thread may still be reading a pack's file, so its handle stays open
    static auto *packs = new std::vector<Pack>;
    static MountedRawData mounted[MAXIMUM_MOUNTED];
    static std::atomic<std::size_t> mounted_count = 0;

    // The models the map being loaded gets, copied in when Halo allocates its model buffer
    struct PlannedModel {
        std::size_t pack;
        std::uint32_t offset;
    };
    static std::vector<PlannedModel> planned_models;
    static std::uint32_t planned_models_size = 0;

    using GlobalAllocFunction = HGLOBAL (__stdcall *)(UINT flags, SIZE_T size);
    static GlobalAllocFunction *global_alloc = nullptr;
    static const std::uint32_t *tag_data_size = nullptr;

    extern "C" {
        void *buh_cached_tag_array = nullptr;
        const void *buh_tag_data_loaded_continue = nullptr;
        void buh_tag_data_loaded_asm() noexcept;
    }

    static std::uint32_t make_tag_id(std::uint32_t index) noexcept {
        return ((0xE174 + index) & 0xFFFF) << 16 | index;
    }

    static std::uint32_t align(std::uint32_t value, std::uint32_t to) noexcept {
        return (value + to - 1) / to * to;
    }

    static bool read_at(HANDLE file, std::uint64_t offset, void *buffer, std::uint32_t size) noexcept {
        OVERLAPPED position = {};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        return size == 0 || (ReadFile(file, buffer, size, &read, &position) && read == size);
    }

    template<typename T> static bool read_array(HANDLE file, std::uint64_t offset, std::vector<T> &out, std::size_t count) noexcept {
        out.resize(count);
        return read_at(file, offset, out.data(), static_cast<std::uint32_t>(count * sizeof(T)));
    }

    static bool fits(std::uint32_t offset, std::uint32_t size, std::size_t within) noexcept {
        return offset <= within && size <= within - offset;
    }

    // Everything but the raw data, which is read as Halo needs it. Checked so a broken pack can't write outside itself.
    static bool load_pack(const std::string &file, Pack &pack, std::string &error) noexcept {
        pack.file = file;
        pack.handle = CreateFileA(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(pack.handle == INVALID_HANDLE_VALUE) {
            error = "couldn't open " + file;
            return false;
        }
        LARGE_INTEGER file_size;
        GetFileSizeEx(pack.handle, &file_size);
        auto &h = pack.header;
        auto in_file = [&file_size](std::uint32_t offset, std::uint64_t size) {
            return offset + size <= static_cast<std::uint64_t>(file_size.QuadPart);
        };
        if(!read_at(pack.handle, 0, &h, sizeof(h)) || std::memcmp(h.magic, "BUHP", 4) != 0) {
            error = file + " isn't a tag pack";
        }
        else if(h.version != PACK_VERSION) {
            error = file + " is a version " + std::to_string(h.version) + " tag pack; this buh reads version " + std::to_string(PACK_VERSION);
        }
        else if(!in_file(h.tags_offset, h.tag_count * sizeof(PackTag)) || !in_file(h.tag_data_offset, h.tag_data_size) || !in_file(h.raw_offset, h.raw_size)
             || !in_file(h.model_offset, static_cast<std::uint64_t>(h.vertex_size) + h.index_size)
             || !in_file(h.relocations_offset, (static_cast<std::uint64_t>(h.pointer_count) + h.file_offset_count + h.model_vertex_count + h.model_index_count) * 4 + h.tag_id_count * 8ULL)) {
            error = file + " is cut short";
        }
        if(!error.empty()) {
            CloseHandle(pack.handle);
            return false;
        }

        std::uint64_t at = h.relocations_offset;
        bool read = read_array(pack.handle, h.tags_offset, pack.tags, h.tag_count)
            && read_array(pack.handle, h.tag_data_offset, pack.tag_data, h.tag_data_size)
            && read_array(pack.handle, at, pack.pointers, h.pointer_count)
            && read_array(pack.handle, at += h.pointer_count * 4ULL, pack.tag_ids, h.tag_id_count)
            && read_array(pack.handle, at += h.tag_id_count * 8ULL, pack.file_offsets, h.file_offset_count)
            && read_array(pack.handle, at += h.file_offset_count * 4ULL, pack.model_vertices, h.model_vertex_count)
            && read_array(pack.handle, at += h.model_vertex_count * 4ULL, pack.model_indices, h.model_index_count)
            && read_array(pack.handle, h.model_offset, pack.model, static_cast<std::size_t>(h.vertex_size) + h.index_size);
        if(!read) {
            error = "couldn't read " + file;
            CloseHandle(pack.handle);
            return false;
        }

        // Every relocation and tag has to stay inside the pack
        auto size = pack.tag_data.size();
        auto field_ok = [size](std::uint32_t offset) { return offset % 4 == 0 && fits(offset, 4, size); };
        bool valid = std::all_of(pack.pointers.begin(), pack.pointers.end(), field_ok)
            && std::all_of(pack.file_offsets.begin(), pack.file_offsets.end(), field_ok)
            && std::all_of(pack.model_vertices.begin(), pack.model_vertices.end(), field_ok)
            && std::all_of(pack.model_indices.begin(), pack.model_indices.end(), field_ok)
            && std::all_of(pack.tag_ids.begin(), pack.tag_ids.end(), [&](const TagIDRelocation &r) { return field_ok(r.offset) && r.tag < h.tag_count; })
            && std::all_of(pack.tags.begin(), pack.tags.end(), [size](const PackTag &t) { return t.path < size && t.data < size; })
            && h.tag_count > 0 && h.tag_count < MAXIMUM_TAGS;
        if(!valid) {
            error = file + " is damaged";
            CloseHandle(pack.handle);
            return false;
        }
        pack.name.assign(h.name, strnlen(h.name, sizeof(h.name)));
        return true;
    }

    // On Halo's file thread
    static void read_raw_data(std::byte *buffer, std::uint32_t offset, std::uint32_t size) noexcept {
        auto count = mounted_count.load();
        for(std::size_t i = 0; i < count; i++) {
            auto &m = mounted[i];
            if(offset >= m.offset && offset - m.offset < m.size) {
                // A read can run past the data's end (it gets rounded up); there's nothing there
                auto available = std::min(size, m.size - (offset - m.offset));
                if(read_at(m.file, static_cast<std::uint64_t>(m.file_offset) + (offset - m.offset), buffer, available)) {
                    std::memset(buffer + available, 0, size - available);
                    return;
                }
                break;
            }
        }
        std::memset(buffer, 0, size);
    }

    // Where the packs can go: after the map's tag data and its BSPs (which Halo loads into tag space too), or
    // between them, up to the end of the tag space allocation
    static std::uintptr_t find_room(const TagDataHeader &header, std::uint32_t needed) noexcept {
        auto data_end = static_cast<std::uintptr_t>(TAG_DATA_ADDRESS) + *tag_data_size;
        MEMORY_BASIC_INFORMATION info;
        std::uintptr_t space_end = STOCK_TAG_SPACE_END;
        if(VirtualQuery(reinterpret_cast<const void *>(TAG_DATA_ADDRESS), &info, sizeof(info)) && info.State == MEM_COMMIT) {
            space_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        auto bsp_start = space_end, bsp_end = data_end;
        auto *scenario = get_tag(header.scenario_tag);
        if(scenario && readable(scenario->data + SCENARIO_STRUCTURE_BSPS_OFFSET, 8)) {
            auto count = block_count(scenario->data + SCENARIO_STRUCTURE_BSPS_OFFSET);
            auto *bsps = block_elements(scenario->data + SCENARIO_STRUCTURE_BSPS_OFFSET);
            for(std::uint32_t i = 0; i < count && readable(bsps, count * SCENARIO_BSP_SIZE); i++) {
                auto size = *reinterpret_cast<const std::uint32_t *>(bsps + i * SCENARIO_BSP_SIZE + 4);
                auto address = *reinterpret_cast<const std::uint32_t *>(bsps + i * SCENARIO_BSP_SIZE + 8);
                bsp_start = std::min<std::uintptr_t>(bsp_start, address);
                bsp_end = std::max<std::uintptr_t>(bsp_end, address + size);
            }
        }
        for(auto [start, end] : { std::pair { align(static_cast<std::uint32_t>(bsp_end), 0x1000), space_end }, std::pair { align(static_cast<std::uint32_t>(data_end), 0x1000), bsp_start } }) {
            if(start < end && end - start >= needed) {
                return start;
            }
        }
        return 0;
    }

    // Where the packs go when tag space hasn't the room: memory of their own, kept for the next map that needs it.
    // Nothing in a pack has to be in tag space, since every pointer in it is relocated to wherever it is. By the time
    // the next map's tag data is read, Halo is done with the last map's tags, as it is with tag space.
    static std::byte *own_room = nullptr;
    static std::size_t own_room_size = 0;

    static void release_own_room() noexcept {
        if(own_room) {
            VirtualFree(own_room, 0, MEM_RELEASE);
            own_room = nullptr;
            own_room_size = 0;
        }
    }

    static std::uintptr_t take_own_room(std::size_t needed) noexcept {
        if(own_room_size < needed) {
            release_own_room();
            auto size = (needed + 0xFFFF) & ~static_cast<std::size_t>(0xFFFF);
            own_room = static_cast<std::byte *>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            own_room_size = own_room ? size : 0;
        }
        return reinterpret_cast<std::uintptr_t>(own_room);
    }

    // Right after Halo reads the map's tag data, and before it caches where the tag array is
    extern "C" void buh_tag_data_loaded(std::byte *tag_data) noexcept {
        mounted_count = 0;
        planned_models.clear();
        planned_models_size = 0;
        for(auto &pack : *packs) {
            pack.in_map = false;
        }
        if(packs->empty() || is_ui_map(loading_map_name())) {
            release_own_room();
            return;
        }

        auto &header = *reinterpret_cast<TagDataHeader *>(tag_data);
        std::uint32_t map_tags = header.tag_count;
        std::uint32_t pack_tags = 0, blocks = 0;
        std::uint64_t raw = 0;
        for(auto &pack : *packs) {
            pack_tags += pack.header.tag_count;
            blocks += align(pack.header.tag_data_size, 16);
            raw += align(pack.header.raw_size, 16);
        }
        if(raw > 0xFFFFFFFF - VIRTUAL_FILE_OFFSET) {
            console_error("buh: the tag packs have more than 2 GiB of bitmaps and sounds between them, so no packs this time");
            release_own_room();
            return;
        }
        if(map_tags + pack_tags > MAXIMUM_TAGS || packs->size() > MAXIMUM_MOUNTED) {
            console_error(("buh: the map and its tag packs come to " + std::to_string(map_tags + pack_tags) + " tags, more than Halo can have, so no packs this time").c_str());
            release_own_room();
            return;
        }
        auto array_size = (map_tags + pack_tags) * static_cast<std::uint32_t>(sizeof(Tag));
        auto room = find_room(header, blocks + array_size);
        if(room) {
            release_own_room();
        }
        else {
            room = take_own_room(blocks + array_size);
            if(!room) {
                console_error("buh: no memory for the tag packs this time");
                return;
            }
            auto mib = std::to_string((blocks + array_size + 0xFFFFF) / 0x100000);
            console_print(("buh: tag space has no room for the tag packs, so they're in " + mib + " MiB of memory of their own").c_str());
        }

        // Each pack's block, relocated to where it is now
        std::uint32_t next_tag = map_tags, next_raw = VIRTUAL_FILE_OFFSET;
        std::uint32_t next_model = header.model_data_size;
        auto *block = reinterpret_cast<std::byte *>(room);
        auto *new_array = reinterpret_cast<Tag *>(room + blocks);
        std::memcpy(new_array, header.tag_array, map_tags * sizeof(Tag));
        std::unordered_set<std::string> map_paths;
        for(std::uint32_t i = 0; i < map_tags; i++) {
            auto &tag = header.tag_array[i];
            if(readable(tag.path)) {
                map_paths.insert(std::string(reinterpret_cast<const char *>(&tag.primary_class), 4) + tag.path);
            }
        }
        std::size_t duplicates = 0;
        for(std::size_t p = 0; p < packs->size(); p++) {
            auto &pack = (*packs)[p];
            auto address = reinterpret_cast<std::uint32_t>(block);
            auto vertices = next_model, indices = next_model + pack.header.vertex_size;
            std::memcpy(block, pack.tag_data.data(), pack.tag_data.size());
            auto field = [block](std::uint32_t offset) -> std::uint32_t & { return *reinterpret_cast<std::uint32_t *>(block + offset); };
            for(auto offset : pack.pointers) {
                field(offset) += address;
            }
            for(auto &r : pack.tag_ids) {
                field(r.offset) = make_tag_id(next_tag + r.tag);
            }
            for(auto offset : pack.file_offsets) {
                field(offset) += next_raw;
            }
            // Halo reads a part's vertices at its offset into the model buffer, and its indices at the offset into
            // the indices, which start after the map's vertices
            for(auto offset : pack.model_vertices) {
                field(offset) += vertices;
            }
            for(auto offset : pack.model_indices) {
                field(offset) += indices - header.vertex_size;
            }
            for(std::uint32_t t = 0; t < pack.header.tag_count; t++) {
                auto &entry = pack.tags[t];
                auto &tag = new_array[next_tag + t];
                tag.primary_class = entry.classes[0];
                tag.secondary_class = entry.classes[1];
                tag.tertiary_class = entry.classes[2];
                tag.id = make_tag_id(next_tag + t);
                tag.path = reinterpret_cast<const char *>(block + entry.path);
                tag.data = block + entry.data;
                tag.indexed = 0;
                tag.padding = 0;
                duplicates += map_paths.count(std::string(reinterpret_cast<const char *>(&tag.primary_class), 4) + tag.path);
            }
            mounted[p] = { next_raw, pack.header.raw_size, pack.handle, pack.header.raw_offset };
            if(!pack.model.empty()) {
                planned_models.push_back({ p, vertices });
            }
            pack.in_map = true;
            next_tag += pack.header.tag_count;
            next_raw += align(pack.header.raw_size, 16);
            next_model += static_cast<std::uint32_t>(pack.model.size());
            block += align(pack.header.tag_data_size, 16);
        }
        planned_models_size = next_model - header.model_data_size;
        mounted_count = packs->size();
        header.tag_array = new_array;
        header.tag_count = next_tag;
        if(duplicates) {
            console_print(("buh: " + std::to_string(duplicates) + " tag pack tags have the same path as a map tag; looking one up by path finds the map's").c_str());
        }
    }

    // Stands in for GlobalAlloc when Halo allocates the buffer it reads the map's model data into, which it then
    // makes every model part's vertex and index buffers out of and frees
    static HGLOBAL __stdcall allocate_model_data(UINT flags, SIZE_T size) {
        auto extra = planned_models_size;
        auto *buffer = static_cast<std::byte *>((*global_alloc)(flags, size + extra));
        if(buffer && extra) {
            for(auto &planned : planned_models) {
                auto &model = (*packs)[planned.pack].model;
                std::memcpy(buffer + planned.offset, model.data(), model.size());
            }
        }
        planned_models.clear();
        planned_models_size = 0;
        return buffer;
    }

    static void pack_command(const std::vector<std::string> &arguments) {
        Pack pack;
        std::string error;
        if(!load_pack(arguments[0], pack, error)) {
            console_error(("buh: " + error).c_str());
            return;
        }
        auto existing = std::find_if(packs->begin(), packs->end(), [&](const Pack &other) { return _stricmp(other.file.c_str(), pack.file.c_str()) == 0; });
        auto index = static_cast<std::size_t>(existing - packs->begin());
        if(existing != packs->end()) {
            // The old handle stays open in case Halo is still reading through it
            *existing = std::move(pack);
        }
        else {
            packs->push_back(std::move(pack));
        }
        auto &added = (*packs)[index];
        console_print(("buh: tag pack " + added.name + " (" + std::to_string(added.header.tag_count) + " tags) is added from the next map on").c_str());
    }

    static void remove_command(const std::vector<std::string> &arguments) {
        auto before = packs->size();
        packs->erase(std::remove_if(packs->begin(), packs->end(), [&](const Pack &pack) { return _stricmp(pack.file.c_str(), arguments[0].c_str()) == 0; }), packs->end());
        console_print(before == packs->size() ? "buh: no such tag pack" : "buh: removed from the next map on");
    }

    static void list_command(const std::vector<std::string> &) {
        if(packs->empty()) {
            console_print("buh: no tag packs");
        }
        for(auto &pack : *packs) {
            console_print((pack.file + ": " + pack.name + ", " + std::to_string(pack.header.tag_count) + " tags" + (pack.in_map ? ", in this map" : "")).c_str());
        }
    }

    template<typename T> static T *read_address(const std::byte *at) noexcept {
        std::uint32_t address;
        std::memcpy(&address, at, sizeof(address));
        return reinterpret_cast<T *>(address);
    }

    void set_up_tag_packs() noexcept {
        // Right after the tag data is read: (Chimera's indexed tag hook, or mov [tag data header], edi;) mov eax, [edi];
        // mov [tag array], eax; ...; mov ecx, [tag data size]; add ecx, edi
        auto *loaded = find_signature({ -1, -1, -1, -1, -1, -1, 0x8B, 0x07, 0xA3, -1, -1, -1, -1, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x88, 0x1D, -1, -1, -1, -1, 0x0F, 0x85, -1, -1, -1, -1, 0x8B, 0x0D, -1, -1, -1, -1, 0x03, 0xCF });
        // The model buffer's allocation: call [GlobalAlloc]; mov ecx, [ebx + 0x20]; mov edx, [ebx + 0x14]; push edi; mov ebp, eax
        auto *model_alloc = find_signature({ 0xFF, 0x15, -1, -1, -1, -1, 0x8B, 0x4B, 0x20, 0x8B, 0x53, 0x14, 0x57, 0x8B, 0xE8 });
        if(!loaded || !model_alloc) {
            return;
        }
        tag_data_size = read_address<const std::uint32_t>(loaded + 0x22);
        global_alloc = read_address<GlobalAllocFunction>(model_alloc + 2);
        buh_cached_tag_array = read_address<void>(loaded + 9);
        buh_tag_data_loaded_continue = loaded + 13;
        write_jump(loaded + 6, reinterpret_cast<const void *>(buh_tag_data_loaded_asm), 7);
        write_call(model_alloc, reinterpret_cast<const void *>(allocate_model_data), 6);
        set_virtual_read_handler(read_raw_data);

        add_console_command("buh_tag_pack", "<file>", 1, 1, pack_command);
        add_console_command("buh_tag_pack_remove", "<file>", 1, 1, remove_command);
        add_console_command("buh_tag_packs", "", 0, 0, list_command);
    }
}
