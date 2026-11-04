// SPDX-License-Identifier: GPL-3.0-only
//
// Halo's scripts, from outside them. Halo keeps a table of its script functions' definitions, each with the type it
// returns, and one of its own globals' (hs_external_global_definition: name, type, where it's kept); both are found
// from an entry buh knows by name. A script can only name an object the scenario named, and Halo looks each name up
// in a list of objects by name index (object_name_list, which object_index_from_name_index reads). Names added past
// the scenario's own for a moment, each with its object in that list, let a script be given any object; the scenario's
// block of names is pointed at a copy with them added, and put back after.

#include "hs.hpp"
#include "halo.hpp"
#include "memory.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

// The scenario's object names, each the name, the type of object it names (which a script's compiler checks it
// against), and which of the scenario's objects it is
#define SCENARIO_OBJECT_NAMES 0x204
#define OBJECT_NAME_SIZE 0x24
#define OBJECT_NAME_TYPE 0x20
#define OBJECT_NAME_OBJECT 0x22
#define OBJECT_NAME_LENGTH 32

// Halo's list of objects by name has room for this many
#define MAXIMUM_OBJECT_NAMES 512

#define OBJECT_TYPE 0xB4

#define BOUND_NAME_PREFIX "buh_object_"

namespace Buh {
    struct ExternalGlobal {
        std::int16_t type;
        void *address;
    };
    static auto *functions = new std::unordered_map<std::string, std::int16_t>;
    static auto *globals = new std::unordered_map<std::string, ExternalGlobal>;

    // Where Halo keeps the address of its list of objects by name index
    static std::uint32_t *const *object_name_list = nullptr;

    template<typename T> static T read(const std::byte *at) noexcept {
        T value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }

    // A name in Halo's image: printable, and not too long
    static bool image_string(const char *string) noexcept {
        for(std::size_t i = 0; i < 64 && image_contains(string + i, 1); i++) {
            auto c = string[i];
            if(c == 0) {
                return i > 0;
            }
            if(c < 0x20 || c > 0x7E) {
                return false;
            }
        }
        return false;
    }

    // hs_function_definition: return type, flags, then pointers to its name, parser, evaluator... One of the two is
    // code, and the other code or nothing (cond has no evaluator: its parser makes it into ifs)
    static bool function_definition(const std::byte *definition) noexcept {
        if(!image_contains(definition, 0x18)) {
            return false;
        }
        auto type = read<std::int16_t>(definition);
        auto *parse = read<const std::byte *>(definition + 8);
        auto *evaluate = read<const std::byte *>(definition + 0xC);
        return type >= 0 && type < 64 && image_string(read<const char *>(definition + 4)) && (parse || evaluate)
            && (!parse || image_contains(parse, 1, true)) && (!evaluate || image_contains(evaluate, 1, true));
    }

    // hs_external_global_definition: a pointer to its name, its type, and where it's kept (nothing, for one of PC's
    // debug globals)
    static bool global_definition(const std::byte *definition) noexcept {
        if(!image_contains(definition, 0xC)) {
            return false;
        }
        auto type = read<std::int16_t>(definition + 4);
        auto *address = read<const std::byte *>(definition + 8);
        return type > HS_TYPE_VOID && type < 64 && image_string(read<const char *>(definition))
            && (!address || image_contains(address, 1));
    }

    // Every definition in a table of pointers to them, from one that's in it: back to the first, on to the last. One
    // pointer to it on its own isn't a table.
    static std::vector<const std::byte *> definition_table(const std::byte *known, bool (*valid)(const std::byte *)) noexcept {
        for(auto *slot : find_in_image(&known, sizeof(known))) {
            auto *first = slot;
            while(image_contains(first - 4, 4) && valid(read<const std::byte *>(first - 4))) {
                first -= 4;
            }
            auto *last = slot;
            while(image_contains(last + 4, 4) && valid(read<const std::byte *>(last + 4))) {
                last += 4;
            }
            if(last - first >= 15 * 4) {
                std::vector<const std::byte *> table;
                for(auto *at = first; at <= last; at += 4) {
                    table.push_back(read<const std::byte *>(at));
                }
                return table;
            }
        }
        return {};
    }

    bool hs_available() noexcept {
        return !functions->empty() && !globals->empty();
    }

    int hs_function_type(const char *name) noexcept {
        auto found = functions->find(name);
        return found == functions->end() ? -1 : found->second;
    }

    bool hs_global(const char *name, std::int16_t &type, void *&address) noexcept {
        auto found = globals->find(name);
        if(found == globals->end() || !found->second.address) {
            return false;
        }
        type = found->second.type;
        address = found->second.address;
        return true;
    }

    HsGlobal hs_scratch_global(std::int16_t type) noexcept {
        // Debug and tuning globals nothing reads while a script runs; then any other of the type
        static const char *const preferred[] = { "debug_frustum", "object_light_secondary_scale", "rasterizer_frame_drop_ms", "debug_score" };
        for(auto *name : preferred) {
            auto found = globals->find(name);
            if(found != globals->end() && found->second.type == type && found->second.address) {
                return { found->first.c_str(), found->second.address };
            }
        }
        for(auto &[name, global] : *globals) {
            if(global.type == type && global.address) {
                return { name.c_str(), global.address };
            }
        }
        return { nullptr, nullptr };
    }

    // The scenario's names as they were, while objects are bound
    static struct {
        std::byte *block;
        std::uint32_t count;
        std::byte *elements;
        std::size_t added;
    } bound = {};
    static auto *names = new std::vector<std::byte>;

    std::string hs_bound_name(std::size_t index) {
        return BOUND_NAME_PREFIX + std::to_string(index + 1);
    }

    const char *hs_bind_objects(const std::uint32_t *objects, std::size_t count) noexcept {
        if(bound.added) {
            return "objects are bound to names already";
        }
        if(!object_name_list || !*object_name_list) {
            return "buh couldn't find Halo's list of objects by name";
        }
        auto *scenario = scenario_data();
        if(!scenario) {
            return "no map is loaded";
        }
        auto *block = scenario + SCENARIO_OBJECT_NAMES;
        auto old_count = block_count(block);
        auto *old_elements = block_elements(block);
        if(old_count + count > MAXIMUM_OBJECT_NAMES) {
            return "the scenario has too many object names to add more";
        }
        if(old_count && !readable(old_elements, old_count * OBJECT_NAME_SIZE)) {
            return "the scenario's object names can't be read";
        }
        std::vector<std::int16_t> types;
        for(std::size_t i = 0; i < count; i++) {
            auto *datum = object_datum(objects[i]);
            if(!datum) {
                return "no such object";
            }
            types.push_back(read<std::int16_t>(datum + OBJECT_TYPE));
        }
        // A script finds the scenario's own name first
        for(std::uint32_t i = 0; i < old_count; i++) {
            if(std::strncmp(reinterpret_cast<const char *>(old_elements + i * OBJECT_NAME_SIZE), BOUND_NAME_PREFIX, sizeof(BOUND_NAME_PREFIX) - 1) == 0) {
                return "the scenario has an object name of buh's own (" BOUND_NAME_PREFIX "...)";
            }
        }

        names->assign(old_elements, old_elements + old_count * OBJECT_NAME_SIZE);
        names->resize((old_count + count) * OBJECT_NAME_SIZE);
        auto *list = *object_name_list;
        for(std::size_t i = 0; i < count; i++) {
            auto *element = names->data() + (old_count + i) * OBJECT_NAME_SIZE;
            std::memset(element, 0, OBJECT_NAME_SIZE);
            std::snprintf(reinterpret_cast<char *>(element), OBJECT_NAME_LENGTH, "%s", hs_bound_name(i).c_str());
            std::int16_t no_object = -1;
            std::memcpy(element + OBJECT_NAME_TYPE, &types[i], sizeof(types[i]));
            std::memcpy(element + OBJECT_NAME_OBJECT, &no_object, sizeof(no_object));
            list[old_count + i] = objects[i];
        }
        auto new_count = static_cast<std::uint32_t>(old_count + count);
        auto *new_elements = names->data();
        std::memcpy(block, &new_count, sizeof(new_count));
        std::memcpy(block + 4, &new_elements, sizeof(new_elements));
        bound = { block, old_count, old_elements, count };
        return nullptr;
    }

    void hs_unbind_objects() noexcept {
        if(!bound.added) {
            return;
        }
        if(auto *list = object_name_list ? *object_name_list : nullptr) {
            for(std::size_t i = 0; i < bound.added; i++) {
                list[bound.count + i] = 0xFFFFFFFF;
            }
        }
        std::memcpy(bound.block, &bound.count, sizeof(bound.count));
        std::memcpy(bound.block + 4, &bound.elements, sizeof(bound.elements));
        bound = {};
    }

    void set_up_hs() noexcept {
        if(auto *known = script_function("structure_bsp_index", "returns the current structure bsp index")) {
            for(auto *definition : definition_table(known, function_definition)) {
                functions->emplace(read<const char *>(definition + 4), read<std::int16_t>(definition));
            }
        }

        // The whole name, with the NUL before it too
        static const char known_global[] = "\0cheat_deathless_player";
        for(auto *found : find_in_image(known_global, sizeof(known_global))) {
            auto *name = found + 1;
            for(auto *definition : find_in_image(&name, sizeof(name))) {
                if(!global_definition(definition)) {
                    continue;
                }
                for(auto *global : definition_table(definition, global_definition)) {
                    globals->emplace(read<const char *>(global), ExternalGlobal { read<std::int16_t>(global + 4), read<void *>(global + 8) });
                }
                if(!globals->empty()) {
                    break;
                }
            }
            if(!globals->empty()) {
                break;
            }
        }

        // object_index_from_name_index(ax): test ax, ax; jl; cmp ax, 0x200; jge; mov ecx, [object_name_list];
        // movsx eax, ax; mov eax, [ecx + eax * 4]; ret; or eax, -1; ret
        if(auto *lookup = find_signature({ 0x66, 0x85, 0xC0, 0x7C, -1, 0x66, 0x3D, 0x00, 0x02, 0x7D, -1, 0x8B, 0x0D, -1, -1, -1, -1, 0x0F, 0xBF, 0xC0, 0x8B, 0x04, 0x81, 0xC3, 0x83, 0xC8, 0xFF, 0xC3 })) {
            object_name_list = read<std::uint32_t *const *>(lookup + 13);
        }
    }
}
