// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_MEMORY_HPP
#define BUH_MEMORY_HPP

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace Buh {
    /**
     * Find the first match of a byte pattern in Halo's code. -1 matches any byte.
     * @return the match, or nullptr if there is none
     */
    std::byte *find_signature(std::initializer_list<std::int16_t> pattern) noexcept;

    /**
     * Find every occurrence of a 32-bit value in Halo's code.
     */
    std::vector<std::byte *> find_value_in_code(std::uint32_t value) noexcept;

    /**
     * Overwrite memory even if it is read-only.
     */
    template<typename T> inline void overwrite(void *pointer, const T *data, std::size_t length) noexcept {
        DWORD old_protection;
        VirtualProtect(pointer, length * sizeof(T), PAGE_EXECUTE_READWRITE, &old_protection);
        std::copy(data, data + length, reinterpret_cast<T *>(pointer));
        VirtualProtect(pointer, length * sizeof(T), old_protection, &old_protection);
    }

    template<typename T> inline void overwrite(void *pointer, const T &data) noexcept {
        overwrite(pointer, &data, 1);
    }

    /**
     * Overwrite memory with nops.
     */
    void fill_with_nops(std::byte *address, std::size_t length) noexcept;

    /**
     * Point a call rel32 at a different function.
     */
    void retarget_call(std::byte *call, const void *target) noexcept;
}

#endif
