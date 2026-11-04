// SPDX-License-Identifier: GPL-3.0-only

#include "memory.hpp"

#include <cstring>

namespace Buh {
    struct CodeSection {
        std::byte *address;
        std::size_t size;
        bool code;
    };

    // A module's sections: its code, or all of them
    static std::vector<CodeSection> module_sections(HMODULE module, bool code_only) noexcept {
        std::vector<CodeSection> found;
        auto *base = reinterpret_cast<std::byte *>(module);
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
        auto *section = reinterpret_cast<const IMAGE_SECTION_HEADER *>(reinterpret_cast<const std::byte *>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);
        for(std::size_t i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            bool code = section->Characteristics & IMAGE_SCN_MEM_EXECUTE;
            if(code || !code_only) {
                std::size_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
                found.push_back({ base + section->VirtualAddress, size, code });
            }
        }
        return found;
    }

    static std::vector<CodeSection> module_code_sections(HMODULE module) noexcept {
        return module_sections(module, true);
    }

    static const std::vector<CodeSection> &code_sections() noexcept {
        static const auto sections = module_code_sections(GetModuleHandle(nullptr));
        return sections;
    }

    static const std::vector<CodeSection> &image_sections() noexcept {
        static const auto sections = module_sections(GetModuleHandle(nullptr), false);
        return sections;
    }

    std::vector<std::byte *> find_in_image(const void *bytes, std::size_t size) noexcept {
        std::vector<std::byte *> sites;
        auto *wanted = static_cast<const std::byte *>(bytes);
        for(auto &section : image_sections()) {
            for(std::size_t offset = 0; offset + size <= section.size; offset++) {
                if(std::memcmp(section.address + offset, wanted, size) == 0) {
                    sites.push_back(section.address + offset);
                }
            }
        }
        return sites;
    }

    bool image_contains(const void *address, std::size_t size, bool code) noexcept {
        auto *at = static_cast<const std::byte *>(address);
        for(auto &section : image_sections()) {
            if(at >= section.address && at + size <= section.address + section.size && (section.code || !code)) {
                return true;
            }
        }
        return false;
    }

    std::byte *find_signature(std::initializer_list<std::int16_t> pattern) noexcept {
        auto matches = [&pattern](const std::byte *at) {
            return std::equal(pattern.begin(), pattern.end(), at, [](std::int16_t expected, std::byte actual) {
                return expected == -1 || expected == static_cast<std::int16_t>(actual);
            });
        };
        for(auto &section : code_sections()) {
            for(std::size_t offset = 0; offset + pattern.size() <= section.size; offset++) {
                if(matches(section.address + offset)) {
                    return section.address + offset;
                }
            }
        }
        return nullptr;
    }

    std::vector<std::byte *> find_value_in_code(std::uint32_t value) noexcept {
        std::vector<std::byte *> sites;
        for(auto &section : code_sections()) {
            for(std::size_t offset = 0; offset + sizeof(value) <= section.size; offset++) {
                std::uint32_t candidate;
                std::memcpy(&candidate, section.address + offset, sizeof(candidate));
                if(candidate == value) {
                    sites.push_back(section.address + offset);
                }
            }
        }
        return sites;
    }

    std::vector<std::byte *> find_calls(const void *function, HMODULE module) noexcept {
        std::vector<std::byte *> calls;
        for(auto &section : module ? module_code_sections(module) : code_sections()) {
            for(std::size_t offset = 0; offset + 5 <= section.size; offset++) {
                auto *at = section.address + offset;
                if(*at == std::byte { 0xE8 } && branch_target(at) == function) {
                    calls.push_back(at);
                }
            }
        }
        return calls;
    }

    void fill_with_nops(std::byte *address, std::size_t length) noexcept {
        std::vector<std::uint8_t> nops(length, 0x90);
        overwrite(address, nops.data(), length);
    }

    void retarget_call(std::byte *call, const void *target) noexcept {
        auto relative = reinterpret_cast<std::uintptr_t>(target) - (reinterpret_cast<std::uintptr_t>(call) + 5);
        overwrite(call + 1, static_cast<std::uint32_t>(relative));
    }

    std::byte *branch_target(const std::byte *branch) noexcept {
        std::int32_t relative;
        std::memcpy(&relative, branch + 1, sizeof(relative));
        return const_cast<std::byte *>(branch) + 5 + relative;
    }

    static void write_branch(std::uint8_t opcode, std::byte *at, const void *target, std::size_t length) noexcept {
        std::vector<std::uint8_t> code(length, 0x90);
        code[0] = opcode;
        auto relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target) - (reinterpret_cast<std::uintptr_t>(at) + 5));
        std::memcpy(code.data() + 1, &relative, sizeof(relative));
        overwrite(at, code.data(), length);
    }

    void write_jump(std::byte *at, const void *target, std::size_t length) noexcept {
        write_branch(0xE9, at, target, length);
    }

    void write_call(std::byte *at, const void *target, std::size_t length) noexcept {
        write_branch(0xE8, at, target, length);
    }

    const std::byte *buh_resource(const char *name, std::size_t &size) noexcept {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&buh_resource), &module);
        // RT_RCDATA, as a narrow string
        auto *resource = module ? FindResourceA(module, name, MAKEINTRESOURCEA(10)) : nullptr;
        auto *data = resource ? static_cast<const std::byte *>(LockResource(LoadResource(module, resource))) : nullptr;
        size = data ? SizeofResource(module, resource) : 0;
        return data;
    }
}
