// SPDX-License-Identifier: GPL-3.0-only

#include "memory.hpp"

#include <cstring>

namespace Buh {
    struct CodeSection {
        std::byte *address;
        std::size_t size;
    };

    static const std::vector<CodeSection> &code_sections() noexcept {
        static const auto sections = [] {
            std::vector<CodeSection> found;
            auto *base = reinterpret_cast<std::byte *>(GetModuleHandle(nullptr));
            auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
            auto *section = IMAGE_FIRST_SECTION(nt);
            for(std::size_t i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
                if(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                    std::size_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
                    found.push_back({ base + section->VirtualAddress, size });
                }
            }
            return found;
        }();
        return sections;
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

    void fill_with_nops(std::byte *address, std::size_t length) noexcept {
        std::vector<std::uint8_t> nops(length, 0x90);
        overwrite(address, nops.data(), length);
    }

    void retarget_call(std::byte *call, const void *target) noexcept {
        auto relative = reinterpret_cast<std::uintptr_t>(target) - (reinterpret_cast<std::uintptr_t>(call) + 5);
        overwrite(call + 1, static_cast<std::uint32_t>(relative));
    }
}
