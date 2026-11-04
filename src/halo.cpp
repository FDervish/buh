// SPDX-License-Identifier: GPL-3.0-only

#include "halo.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cstring>
#include <string>

// The tag data header is always here on Custom Edition, and Chimera grows tag space to 64 MiB
#define TAG_DATA_ADDRESS 0x40440000
#define TAG_SPACE_SIZE 0x4000000

// The object table: its slot count and its slots, each a salt and the object
#define TABLE_SIZE 0x2E
#define TABLE_ELEMENTS 0x34
#define OBJECT_SLOT_SIZE 0xC
#define OBJECT_SLOT_OBJECT 0x8

// An object's origin, its bounding sphere's centre, and the object it's attached to
#define OBJECT_POSITION 0x5C
#define OBJECT_CENTER 0xA0
#define OBJECT_PARENT 0x11C

// What a collision test hit: what kind of thing, then (past where along the ray, and where in the level it started and
// ended) the point, the surface's plane, its material and the object
#define COLLISION_RESULT_SIZE 0x50
#define COLLISION_RESULT_TYPE 0x00
#define COLLISION_RESULT_POINT 0x18
#define COLLISION_RESULT_PLANE 0x24
#define COLLISION_RESULT_MATERIAL 0x34
#define COLLISION_RESULT_OBJECT 0x38
#define COLLISION_RESULT_WATER 0
#define COLLISION_RESULT_OBJECT_HIT 3

namespace Buh {
    static IDirect3DDevice9 **device = nullptr;
    static std::uint8_t **game_time_globals = nullptr;
    static const std::uint8_t *console = nullptr;
    static const std::int16_t *server_type = nullptr;
    static const std::byte *const *object_table = nullptr;

    // rasterizer_model_begin's parameters while a model is drawn, null otherwise: its geometry flags, then the
    // identifier it was given
    static const std::byte *const *model_parameters = nullptr;
    #define MODEL_PARAMETERS_IDENTIFIER 0x4

    // The parameters' first byte, whose top bit is first person: rasterizer_model_begin() then sets the projection's
    // near and far to the first person ones, and stencil mode 1 (always, replacing with 1)
    #define MODEL_PARAMETERS_FLAGS 0x0
    #define MODEL_FLAG_FIRST_PERSON 0x80

    // The near and far clip distances first person models are drawn with
    static const float *first_person_near = nullptr;
    static const float *first_person_far = nullptr;

    using CollisionTestVector = bool (*)(std::uint32_t flags, const float *point, const float *vector, std::uint32_t ignore, std::byte *result);
    static CollisionTestVector collision_test_vector = nullptr;
    // The loaded level's collision geometry, or nullptr between maps
    static const void *const *collision_bsp = nullptr;
    // Which structure BSP is loaded, and its tag data
    static const std::int16_t *structure_bsp_index_global = nullptr;
    static const std::byte *const *structure_bsp = nullptr;

    // The dynamic lights gathered for the window being drawn, and how many
    static const std::int32_t *light_count = nullptr;
    static const RenderedLight *light_list = nullptr;
    #define MOST_RENDERED_LIGHTS 128

    struct TagDataHeader {
        Tag *tag_array;
        std::uint32_t scenario_tag;
        std::uint32_t checksum;
        std::uint32_t tag_count;
    };

    static TagDataHeader &tag_data_header() noexcept {
        return *reinterpret_cast<TagDataHeader *>(TAG_DATA_ADDRESS);
    }

    IDirect3DDevice9 *halo_device() noexcept {
        return device ? *device : nullptr;
    }

    bool singleplayer() noexcept {
        return !server_type || *server_type == 0;
    }

    bool game_paused() noexcept {
        auto *globals = game_time_globals ? *game_time_globals : nullptr;
        return globals && globals[2];
    }

    bool console_open() noexcept {
        return console && *console;
    }

    int structure_bsp_index() noexcept {
        return structure_bsp_index_global ? *structure_bsp_index_global : -2;
    }

    void set_game_paused(bool paused) noexcept {
        auto *globals = game_time_globals ? *game_time_globals : nullptr;
        if(!globals) {
            return;
        }
        if(globals[0]) {
            globals[1] = !paused;
        }
        globals[2] = paused;
    }

    bool game_time_stopped() noexcept {
        return singleplayer() && (game_paused() || console_open());
    }

    // The object table's slots and how many there are, or false if there's no table
    static bool object_slots(const std::byte *&slots, std::uint16_t &size) noexcept {
        auto *table = object_table ? *object_table : nullptr;
        if(!table) {
            return false;
        }
        std::memcpy(&size, table + TABLE_SIZE, sizeof(size));
        std::memcpy(&slots, table + TABLE_ELEMENTS, sizeof(slots));
        return slots != nullptr;
    }

    // A slot's salt and object
    static void object_slot(const std::byte *slots, std::uint16_t index, std::uint16_t &salt, const std::byte *&datum) noexcept {
        auto *slot = slots + index * OBJECT_SLOT_SIZE;
        std::memcpy(&salt, slot, sizeof(salt));
        std::memcpy(&datum, slot + OBJECT_SLOT_OBJECT, sizeof(datum));
    }

    const std::byte *object_datum(std::uint32_t object) noexcept {
        const std::byte *slots;
        std::uint16_t size;
        if(object == 0xFFFFFFFF || !object_slots(slots, size) || (object & 0xFFFF) >= size) {
            return nullptr;
        }
        std::uint16_t salt;
        const std::byte *datum;
        object_slot(slots, static_cast<std::uint16_t>(object & 0xFFFF), salt, datum);
        return salt == object >> 16 ? datum : nullptr;
    }

    bool object_position(std::uint32_t object, float (&position)[3]) noexcept {
        auto *datum = object_datum(object);
        if(!datum) {
            return false;
        }
        std::uint32_t parent;
        std::memcpy(&parent, datum + OBJECT_PARENT, sizeof(parent));
        std::memcpy(position, datum + (parent == 0xFFFFFFFF ? OBJECT_POSITION : OBJECT_CENTER), sizeof(position));
        return true;
    }

    std::uint32_t object_id_at(std::uintptr_t address) noexcept {
        const std::byte *slots;
        std::uint16_t size;
        if(!address || !object_slots(slots, size)) {
            return 0xFFFFFFFF;
        }
        for(std::uint16_t index = 0; index < size; index++) {
            std::uint16_t salt;
            const std::byte *datum;
            object_slot(slots, index, salt, datum);
            // A free slot's salt is 0
            if(salt && reinterpret_cast<std::uintptr_t>(datum) == address) {
                return static_cast<std::uint32_t>(salt) << 16 | index;
            }
        }
        return 0xFFFFFFFF;
    }

    bool collision_test(std::uint32_t flags, const float (&origin)[3], const float (&delta)[3], std::uint32_t ignore, CollisionHit &hit) noexcept {
        if(!collision_test_vector || !collision_bsp || !*collision_bsp) {
            return false;
        }
        std::byte result[COLLISION_RESULT_SIZE] = {};
        if(!collision_test_vector(flags, origin, delta, ignore, result)) {
            return false;
        }
        std::int16_t type;
        std::memcpy(&type, result + COLLISION_RESULT_TYPE, sizeof(type));
        hit.what = type == COLLISION_RESULT_WATER ? CollisionHit::WATER : type == COLLISION_RESULT_OBJECT_HIT ? CollisionHit::OBJECT : CollisionHit::LEVEL;
        std::memcpy(hit.point, result + COLLISION_RESULT_POINT, sizeof(hit.point));
        std::memcpy(hit.normal, result + COLLISION_RESULT_PLANE, sizeof(hit.normal));
        std::memcpy(&hit.material, result + COLLISION_RESULT_MATERIAL, sizeof(hit.material));
        hit.object = 0xFFFFFFFF;
        if(hit.what == CollisionHit::OBJECT) {
            std::memcpy(&hit.object, result + COLLISION_RESULT_OBJECT, sizeof(hit.object));
        }
        return true;
    }

    const std::byte *level_structure_bsp() noexcept {
        return structure_bsp ? *structure_bsp : nullptr;
    }

    std::size_t rendered_lights(const RenderedLight *&lights) noexcept {
        if(!light_count || !light_list) {
            return 0;
        }
        lights = light_list;
        return static_cast<std::size_t>(std::clamp(*light_count, 0, MOST_RENDERED_LIGHTS));
    }

    bool in_tag_space(const void *pointer) noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= TAG_DATA_ADDRESS && address < TAG_DATA_ADDRESS + TAG_SPACE_SIZE;
    }

    bool readable(const void *pointer, std::size_t size) noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(pointer);
        if(address == 0) {
            return false;
        }
        if(in_tag_space(pointer) && in_tag_space(reinterpret_cast<const std::byte *>(pointer) + size - 1)) {
            return true;
        }
        // The last readable region found, since lookups tend to stay in one
        thread_local std::uintptr_t known_start = 0, known_end = 0;
        if(address >= known_start && address + size <= known_end) {
            return true;
        }
        MEMORY_BASIC_INFORMATION info;
        const DWORD readable_protection = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if(!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & readable_protection)) {
            return false;
        }
        known_start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        known_end = known_start + info.RegionSize;
        return address + size <= known_end;
    }

    Tag *tag_array() noexcept {
        return tag_data_header().tag_array;
    }

    std::byte *scenario_data() noexcept {
        auto *tag = tag_array() ? get_tag(tag_data_header().scenario_tag) : nullptr;
        return tag ? tag->data : nullptr;
    }

    std::size_t tag_count() noexcept {
        return tag_array() ? tag_data_header().tag_count : 0;
    }

    Tag *get_tag(std::uint32_t id) noexcept {
        auto index = id & 0xFFFF;
        if(id == 0xFFFFFFFF || index >= tag_count()) {
            return nullptr;
        }
        auto *tag = tag_array() + index;
        return tag->id == id && readable(tag->data) ? tag : nullptr;
    }

    Tag *find_tag(const char *path, std::uint32_t tag_class) noexcept {
        for(std::size_t i = 0; i < tag_count(); i++) {
            auto *tag = tag_array() + i;
            // Protected maps can have paths and data that point anywhere
            if(tag->primary_class == tag_class && readable(tag->path) && _stricmp(tag->path, path) == 0 && readable(tag->data)) {
                return tag;
            }
        }
        return nullptr;
    }

    template<typename T> static T *read_address(const std::byte *at) noexcept {
        std::uint32_t address;
        std::memcpy(&address, at, sizeof(address));
        return reinterpret_cast<T *>(address);
    }

    // A script function's definition, found by its name: its return type and flags, then pointers to its name, parser,
    // evaluator, help and usage (hs_function_definition). Its evaluator has to be code; one whose help is the help asked
    // for comes first, in case another table points at the same name.
    const std::byte *script_function(const char *name, const char *help) noexcept {
        // The whole name, with the NUL before it too, so a longer name that ends in it doesn't count
        std::string pattern = std::string(1, '\0') + name;
        pattern.push_back('\0');
        const std::byte *fallback = nullptr;
        for(auto *found : find_in_image(pattern.data(), pattern.size())) {
            auto string = reinterpret_cast<std::uint32_t>(found + 1);
            for(auto *site : find_in_image(&string, sizeof(string))) {
                auto *definition = site - 4;
                if(!image_contains(definition, 0x18) || !image_contains(read_address<const std::byte>(definition + 0xC), 1, true)) {
                    continue;
                }
                auto *help_text = read_address<const char>(definition + 0x10);
                if(image_contains(help_text, std::strlen(help) + 1) && std::strcmp(help_text, help) == 0) {
                    return definition;
                }
                fallback = fallback ? fallback : definition;
            }
        }
        return fallback;
    }

    // The 16-bit global a short piece of code loads first: mov ax, [address]; movsx or movzx r32, word [address]; or
    // mov r16, [address]. Through one call, if it calls a getter for it. It stops at a ret, so as not to read on into
    // the next function; one inside an instruction stops it early, and then it's found nothing, not something wrong.
    static const std::int16_t *word_global_loaded(const std::byte *code, bool through_a_call) noexcept {
        for(std::size_t i = 0; i < 0x30 && image_contains(code + i, 7, true); i++) {
            auto *at = code + i;
            auto byte = [at](std::size_t n) { return std::to_integer<std::uint8_t>(at[n]); };
            if(byte(0) == 0xC3) {
                break;
            }
            const std::int16_t *global = nullptr;
            if(byte(0) == 0x66 && byte(1) == 0xA1) {
                global = read_address<const std::int16_t>(at + 2);
            }
            else if(byte(0) == 0x0F && (byte(1) == 0xBF || byte(1) == 0xB7) && (byte(2) & 0xC7) == 0x05) {
                global = read_address<const std::int16_t>(at + 3);
            }
            else if(byte(0) == 0x66 && byte(1) == 0x8B && (byte(2) & 0xC7) == 0x05) {
                global = read_address<const std::int16_t>(at + 3);
            }
            else if(through_a_call && byte(0) == 0xE8) {
                global = word_global_loaded(branch_target(at), false);
            }
            if(global && image_contains(global, sizeof(*global))) {
                return global;
            }
        }
        return nullptr;
    }

    void set_up_halo() noexcept {
        // Halo's own structure_bsp_index script function, and the global its evaluator reads
        if(auto *definition = script_function("structure_bsp_index", "returns the current structure bsp index")) {
            structure_bsp_index_global = word_global_loaded(read_address<const std::byte>(definition + 0xC), true);
        }

        // Halo's own Bink player, which reads the device from a global
        auto *halo_player = find_signature({ 0x83, 0xEC, 0x68, 0xA1, -1, -1, -1, -1, 0x53, 0x33, 0xDB, 0x3B, 0xC3, 0x89, 0x5C, 0x24, 0x0C, 0x89, 0x5C, 0x24, 0x08, 0x88, 0x5C, 0x24, 0x05, 0xC6, 0x44, 0x24, 0x06, 0x01, 0x88, 0x5C, 0x24, 0x07, 0x0F, 0x85, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x8B, 0x08, 0x53, 0x8D, 0x54, 0x24, 0x0C, 0x52, 0x53, 0x6A, 0x16 });
        auto *time_globals = find_signature({ 0x8B, 0x15, -1, -1, -1, -1, 0x8A, 0x42, 0x02, 0x84, 0xC0, 0x75, 0x22, 0x8B, 0x0D });
        auto *console_enabled = find_signature({ 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x28, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x75, 0x1F, 0x57 });
        auto *server = find_signature({ 0x0F, 0xBF, 0x2D, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x39, 0x1D, -1, -1, -1, -1, 0x75, 0x05 });
        // mov ecx, [object table]; mov edx, [ecx + 0x34]; and eax, 0xFFFF; lea eax, [eax + eax * 2];
        // mov eax, [edx + eax * 4 + 8]; cmp byte [eax + 0x2A3], 0x1C
        auto *objects = find_signature({ 0x8B, 0x0D, -1, -1, -1, -1, 0x8B, 0x51, 0x34, 0x25, 0xFF, 0xFF, 0x00, 0x00, 0x8D, 0x04, 0x40, 0x8B, 0x44, 0x82, 0x08, 0x80, 0xB8, 0xA3, 0x02, 0x00, 0x00, 0x1C });
        if(objects) {
            object_table = read_address<const std::byte *const>(objects + 2);
        }
        // collision_test_vector(flags, point, vector, ignore, result): sub esp, 0x438, its arguments, and whether it
        // tests the level, water or objects at all
        auto *collision = find_signature({ 0x81, 0xEC, 0x38, 0x04, 0x00, 0x00, 0x8B, 0x8C, 0x24, 0x44, 0x04, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x40, 0x04, 0x00, 0x00, 0xF6, 0xC3, 0xE0 });
        // Inside it, the level's collision geometry passed on: mov edx, [collision bsp]; push edi; push ecx;
        // push 0x100; push edx
        auto *bsp = find_signature({ 0x8B, 0x15, -1, -1, -1, -1, 0x57, 0x51, 0x68, 0x00, 0x01, 0x00, 0x00, 0x52, 0x8D, 0x4C, 0x24, -1, 0x89, 0x44, 0x24 });
        if(collision && bsp) {
            collision_test_vector = reinterpret_cast<CollisionTestVector>(collision);
            collision_bsp = read_address<const void *const>(bsp + 2);
        }
        if(halo_player) {
            device = read_address<IDirect3DDevice9 *>(halo_player + 0x29);
        }
        if(time_globals) {
            game_time_globals = read_address<std::uint8_t *>(time_globals + 2);
        }
        if(console_enabled) {
            // The byte before "console enabled" is "console open"
            console = read_address<const std::uint8_t>(console_enabled + 1) - 1;
        }
        if(server) {
            server_type = read_address<const std::int16_t>(server + 3);
        }
        // rasterizer_model_begin keeping its parameters for the model's drawing: mov [parameters], esi; then
        // mov byte [...], 0; mov [...], cl
        if(auto *model_begin = find_signature({ 0x89, 0x35, -1, -1, -1, -1, 0xC6, 0x05, -1, -1, -1, -1, 0x00, 0x88, 0x0D })) {
            model_parameters = read_address<const std::byte *const>(model_begin + 2);
        }
        // The first person clip distances, pushed for the projection to be rebuilt with: mov edx, [far];
        // mov eax, [near]; push edx; push eax; call
        if(auto *clip = find_signature({ 0x8B, 0x15, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x52, 0x50, 0xE8 })) {
            first_person_far = read_address<const float>(clip + 2);
            first_person_near = read_address<const float>(clip + 7);
        }
        // scenario_switch_structure_bsp keeping the loaded structure BSP, then its collision geometry twice:
        // mov [structure bsp], eax; mov eax, [eax + 0xB4]; mov [collision bsp], eax; mov [bsp3d], eax
        if(auto *loaded = find_signature({ 0xA3, -1, -1, -1, -1, 0x8B, 0x80, 0xB4, 0x00, 0x00, 0x00, 0xA3, -1, -1, -1, -1, 0xA3 })) {
            structure_bsp = read_address<const std::byte *const>(loaded + 1);
        }
        // rasterizer_light_submit, inlined where Halo gathers the frame's lights: mov eax, [count]; or ebx, -1;
        // cmp eax, 128; jge; mov ebx, eax; mov edi, ebx; imul edi, edi, 0x38; inc eax; add edi, lights
        if(auto *submit = find_signature({ 0xA1, -1, -1, -1, -1, 0x83, 0xCB, 0xFF, 0x3D, 0x80, 0x00, 0x00, 0x00, 0x7D, -1, 0x8B, 0xD8, 0x8B, 0xFB, 0x6B, 0xFF, 0x38, 0x40, 0x81, 0xC7 })) {
            light_count = read_address<const std::int32_t>(submit + 1);
            light_list = read_address<const RenderedLight>(submit + 25);
        }
    }

    bool drawing_model(std::uint32_t &identifier) noexcept {
        if(!model_parameters || !*model_parameters) {
            return false;
        }
        std::memcpy(&identifier, *model_parameters + MODEL_PARAMETERS_IDENTIFIER, sizeof(identifier));
        return true;
    }

    bool drawing_first_person_model() noexcept {
        return model_parameters && *model_parameters
            && (std::to_integer<std::uint8_t>((*model_parameters)[MODEL_PARAMETERS_FLAGS]) & MODEL_FLAG_FIRST_PERSON) != 0;
    }

    bool first_person_clip(float &near_distance, float &far_distance) noexcept {
        if(!first_person_near || !first_person_far) {
            return false;
        }
        near_distance = *first_person_near;
        far_distance = *first_person_far;
        return near_distance > 0.0F && far_distance > near_distance;
    }
}
