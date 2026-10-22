// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_CONSOLE_HPP
#define BUH_CONSOLE_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace Buh {
    using CommandFunction = void (*)(const std::vector<std::string> &arguments);

    /**
     * Add a console command. Chimera runs its own commands first and passes everything else on to Halo, which is
     * where buh picks them up, from the console, init.txt and Chimera Lua's execute_script alike.
     */
    void add_console_command(const char *name, const char *usage, std::size_t minimum_arguments, std::size_t maximum_arguments, CommandFunction function) noexcept;

    /**
     * Print to Halo's console
     */
    void console_print(const char *message) noexcept;
    void console_error(const char *message) noexcept;

    /**
     * Parse a whole argument
     * @return false if it isn't one
     */
    bool parse_float(const std::string &text, float &value) noexcept;
    bool parse_bool(const std::string &text, bool &value) noexcept;

    /**
     * Hook Halo's console and script evaluator so buh's commands run
     */
    void set_up_console() noexcept;
}

#endif
