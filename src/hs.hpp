// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_HS_HPP
#define BUH_HS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace Buh {
    // Halo's script value types, as its script functions' definitions and its globals give them
    #define HS_TYPE_PASSTHROUGH 3
    #define HS_TYPE_VOID 4
    #define HS_TYPE_BOOLEAN 5
    #define HS_TYPE_REAL 6
    #define HS_TYPE_SHORT 7
    #define HS_TYPE_LONG 8

    /**
     * Whether buh found Halo's script functions and its own globals
     */
    bool hs_available() noexcept;

    /**
     * The type a script function of Halo's returns, or -1 if Halo has none by that name
     */
    int hs_function_type(const char *name) noexcept;

    /**
     * One of Halo's own globals (not a scenario's): its type and where it's kept
     * @return false if there's none by that name, or Halo doesn't keep it anywhere (a debug global, on PC)
     */
    bool hs_global(const char *name, std::int16_t &type, void *&address) noexcept;

    struct HsGlobal {
        const char *name;
        void *address;
    };

    /**
     * A global of Halo's own of a type, for a script to leave a result in for a moment; its name is nullptr if there's
     * none of that type
     */
    HsGlobal hs_scratch_global(std::int16_t type) noexcept;

    /**
     * Give objects names a script can call them by, past the scenario's own: hs_bound_name(0) is the first's, and so
     * on. They stay until hs_unbind_objects(), which puts the scenario back as it was.
     * @return nullptr, or why they couldn't be
     */
    const char *hs_bind_objects(const std::uint32_t *objects, std::size_t count) noexcept;
    std::string hs_bound_name(std::size_t index);
    void hs_unbind_objects() noexcept;

    /**
     * Find Halo's script function and global tables, and its object name list
     */
    void set_up_hs() noexcept;
}

#endif
