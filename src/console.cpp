// SPDX-License-Identifier: GPL-3.0-only

#include "console.hpp"
#include "memory.hpp"

#include <cctype>
#include <cstring>

namespace Buh {
    struct Command {
        const char *name;
        const char *usage;
        std::size_t minimum_arguments;
        std::size_t maximum_arguments;
        CommandFunction function;
    };
    static std::vector<Command> commands;

    // Halo's console colors are alpha, red, green, blue
    static const float PRINT_COLOR[4] = { 1.0F, 1.0F, 1.0F, 1.0F };
    static const float ERROR_COLOR[4] = { 1.0F, 1.0F, 0.2F, 0.2F };

    extern "C" {
        const void *buh_console_print_function = nullptr;
        const void *buh_console_execute_continue = nullptr;
        void buh_console_print_asm(const float *color, const char *message) noexcept;
        void buh_console_execute_asm() noexcept;
    }

    void add_console_command(const char *name, const char *usage, std::size_t minimum_arguments, std::size_t maximum_arguments, CommandFunction function) noexcept {
        commands.push_back({ name, usage, minimum_arguments, maximum_arguments, function });
    }

    void console_print(const char *message) noexcept {
        if(buh_console_print_function) {
            buh_console_print_asm(PRINT_COLOR, message);
        }
    }

    void console_error(const char *message) noexcept {
        if(buh_console_print_function) {
            buh_console_print_asm(ERROR_COLOR, message);
        }
    }

    // Splits on spaces, with double quotes around anything that has them
    static std::vector<std::string> split_arguments(const char *text) {
        std::vector<std::string> arguments;
        while(*text) {
            while(*text == ' ') {
                text++;
            }
            if(!*text) {
                break;
            }
            std::string argument;
            if(*text == '"') {
                text++;
                while(*text && *text != '"') {
                    argument += *text++;
                }
                if(*text == '"') {
                    text++;
                }
            }
            else {
                while(*text && *text != ' ') {
                    argument += *text++;
                }
            }
            arguments.push_back(std::move(argument));
        }
        return arguments;
    }

    static bool same_name(const std::string &a, const char *b) noexcept {
        return a.size() == std::strlen(b) && std::equal(a.begin(), a.end(), b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    // Returns whether text was one of buh's commands, in which case Halo doesn't see it
    extern "C" bool buh_console_execute(const char *text) noexcept {
        auto arguments = split_arguments(text);
        if(arguments.empty()) {
            return false;
        }
        for(auto &command : commands) {
            if(!same_name(arguments[0], command.name)) {
                continue;
            }
            arguments.erase(arguments.begin());
            if(arguments.size() < command.minimum_arguments || arguments.size() > command.maximum_arguments) {
                console_error((std::string("usage: ") + command.name + " " + command.usage).c_str());
            }
            else {
                command.function(arguments);
            }
            return true;
        }
        return false;
    }

    void set_up_console() noexcept {
        // Halo's console executor, with the command in edi. Its first two instructions are mov al, [edi] and
        // sub esp, 0x500, which the hook replays before carrying on.
        auto *execute = find_signature({ 0x8A, 0x07, 0x81, 0xEC, 0x00, 0x05, 0x00, 0x00, 0x3C, 0x3B, 0x74, 0x0E, 0x3C, 0x23, 0x74, 0x0A, 0x3C, 0x2F, 0x75, 0x0F });
        auto *print = find_signature({ 0x83, 0xEC, 0x10, 0x57, 0x8B, 0xF8, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0xC7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x80, 0x3F });
        if(!execute || !print) {
            return;
        }
        buh_console_print_function = print;
        buh_console_execute_continue = execute + 8;

        std::uint8_t jmp[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
        auto relative = reinterpret_cast<std::uintptr_t>(buh_console_execute_asm) - (reinterpret_cast<std::uintptr_t>(execute) + 5);
        std::memcpy(jmp + 1, &relative, sizeof(std::uint32_t));
        overwrite(execute, jmp, sizeof(jmp));
    }
}
