// SPDX-License-Identifier: GPL-3.0-only

#include "console.hpp"
#include "memory.hpp"

#include <cctype>
#include <cstdlib>
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

    // The console's history, which the up and down keys go through: the last 8 lines in a ring
    #define HISTORY_LINES 8
    #define HISTORY_LINE_SIZE 0xFF
    static struct {
        char (*lines)[HISTORY_LINE_SIZE] = nullptr;
        std::int16_t *newest = nullptr;
        std::int16_t *count = nullptr;
        std::int16_t *browsing = nullptr;
    } history;

    // What Halo's tab completion is filling in: what's typed, where the names it finds go, how many it has and how
    // many fit. Which of its searches it runs (functions, globals and so on) is a bit each, functions being the 4th.
    #define TAB_COMPLETION_FUNCTIONS (1 << 3)
    static struct {
        const char **typed = nullptr;
        const char ***found = nullptr;
        std::int16_t *count = nullptr;
        std::int16_t *maximum = nullptr;
    } completion;

    extern "C" {
        const void *buh_console_print_function = nullptr;
        const void *buh_console_execute_continue = nullptr;
        const void *buh_script_evaluate_continue = nullptr;
        const void *buh_tab_completion_continue = nullptr;
        const std::int16_t *buh_tab_completion_count = nullptr;
        void buh_console_print_asm(const float *color, const char *message) noexcept;
        void buh_console_execute_asm() noexcept;
        void buh_script_evaluate_asm() noexcept;
        void buh_tab_completion_asm() noexcept;
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

    bool parse_float(const std::string &text, float &value) noexcept {
        char *end;
        value = std::strtof(text.c_str(), &end);
        return end != text.c_str() && *end == '\0';
    }

    bool parse_bool(const std::string &text, bool &value) noexcept {
        if(text == "1" || text == "true" || text == "on") {
            value = true;
        }
        else if(text == "0" || text == "false" || text == "off") {
            value = false;
        }
        else {
            return false;
        }
        return true;
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

    // What Halo's console executor does with each line before running it, which buh's commands are taken away
    // before. Chimera's commands get it by letting the executor carry on, its "can't be executed" error muted.
    static void add_to_history(const char *text) noexcept {
        if(!history.lines) {
            return;
        }
        *history.newest = static_cast<std::int16_t>((*history.newest + 1) % HISTORY_LINES);
        auto *line = history.lines[*history.newest];
        std::strncpy(line, text, HISTORY_LINE_SIZE - 1);
        line[HISTORY_LINE_SIZE - 1] = '\0';
        if(*history.count < HISTORY_LINES) {
            ++*history.count;
        }
        *history.browsing = -1;
    }

    // A line from the console or init.txt
    extern "C" bool buh_console_line(const char *text) noexcept {
        if(!buh_console_execute(text)) {
            return false;
        }
        add_to_history(text);
        return true;
    }

    // After Halo's tab completion has searched what it was asked to, before it sorts what it found: buh's commands
    // starting with what's typed, matched the way Halo matches its own (ignoring case)
    extern "C" void buh_tab_completion(std::uint32_t searches) noexcept {
        if(!(searches & TAB_COMPLETION_FUNCTIONS) || !*completion.found || !*completion.typed) {
            return;
        }
        auto *found = *completion.found;
        auto &count = *completion.count;
        auto length = std::strlen(*completion.typed);
        for(auto &command : commands) {
            if(count >= *completion.maximum) {
                break;
            }
            if(_strnicmp(command.name, *completion.typed, length) != 0) {
                continue;
            }
            bool already = false;
            for(std::int16_t i = 0; i < count && !already; i++) {
                already = _stricmp(found[i], command.name) == 0;
            }
            if(!already) {
                found[count++] = command.name;
            }
        }
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
        write_jump(execute, reinterpret_cast<const void *>(buh_console_execute_asm), 8);

        // Where the executor puts the line in the history: the newest line's index, the lines, how many there are
        // and where the up and down keys are
        auto *record = find_signature({ 0x0F, 0xBF, 0x05, -1, -1, -1, -1, 0x40, 0x25, 0x07, 0x00, 0x00, 0x80, 0x79, 0x05, 0x48, 0x83, 0xC8, 0xF8, 0x40, 0x66, 0xA3, -1, -1, -1, -1, 0x0F, 0xBF, 0xC0, 0x69, 0xC0, 0xFF, 0x00, 0x00, 0x00, 0x8B, 0xD7, 0x05, -1, -1, -1, -1, 0x8D, 0x64, 0x24, 0x00, 0x8A, 0x0A, 0x42, 0x88, 0x08, 0x40, 0x3A, 0xCB, 0x75, 0xF6, 0x0F, 0xBF, 0x05, -1, -1, -1, -1, 0x40, 0x83, 0xF8, 0x08, 0x66, 0xC7, 0x05, -1, -1, -1, -1, 0x08, 0x00, 0x7F, 0x06, 0x66, 0xA3, -1, -1, -1, -1, 0x8B, 0x0D, -1, -1, -1, -1, 0x0F, 0xBF, 0x05, -1, -1, -1, -1, 0x33, 0xD2, 0x3B, 0xCB, 0x66, 0xC7, 0x05, -1, -1, -1, -1, 0xFF, 0xFF });
        auto address = [record](std::size_t offset) {
            std::uint32_t value;
            std::memcpy(&value, record + offset, sizeof(value));
            return value;
        };
        if(record && address(0x3) == address(0x16) && address(0x3B) == address(0x46) && address(0x3B) == address(0x50)) {
            history.newest = reinterpret_cast<std::int16_t *>(address(0x3));
            history.lines = reinterpret_cast<char (*)[HISTORY_LINE_SIZE]>(address(0x26));
            history.count = reinterpret_cast<std::int16_t *>(address(0x3B));
            history.browsing = reinterpret_cast<std::int16_t *>(address(0x68));
        }

        // Halo's script evaluator, which the console executor ends in and Chimera's Lua execute_script calls
        // directly, so scripts can run buh's commands too: sub esp, 0x80C; push ebx; push ebp; ...
        auto *evaluate = find_signature({ 0x81, 0xEC, 0x0C, 0x08, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xAC, 0x24, 0x18, 0x08, 0x00, 0x00, 0x68, 0x00, 0x04, 0x00, 0x00 });
        if(evaluate) {
            buh_script_evaluate_continue = evaluate + 7;
            write_jump(evaluate, reinterpret_cast<const void *>(buh_script_evaluate_asm), 7);
        }

        // Halo's tab completion search: it keeps how many it found, how many fit, where they go and what's typed, runs
        // a search for each bit set in its first argument, then sorts them, starting with movsx edx, [count]
        auto *complete = find_signature({ 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x56, 0x33, 0xF6, 0x3B, 0xC6, 0x57, 0x66, 0x89,
            0x35, -1, -1, -1, -1, 0x66, 0x89, 0x0D, -1, -1, -1, -1, 0x89, 0x2D, -1, -1, -1, -1, 0xA3, -1, -1, -1, -1, 0x75, 0x0A,
            0xC7, 0x05, -1, -1, -1, -1, -1, -1, -1, -1, 0x66, 0x89, 0x15, -1, -1, -1, -1, 0xBF, -1, -1, -1, -1, 0xBB, 0x12, 0x00,
            0x00, 0x00, 0x8B, 0xCE, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE0, 0x8B, 0x4C, 0x24, 0x14, 0x85, 0xC1, 0x74, 0x02,
            0xFF, 0x17, 0x46, 0x83, 0xC7, 0x04, 0x4B, 0x75, 0xE6, 0x0F, 0xBF, 0x15, -1, -1, -1, -1 });
        auto complete_address = [complete](std::size_t offset) {
            std::uint32_t value;
            std::memcpy(&value, complete + offset, sizeof(value));
            return value;
        };
        if(complete && complete_address(0x0F) == complete_address(0x5F)) {
            completion.count = reinterpret_cast<std::int16_t *>(complete_address(0x0F));
            completion.maximum = reinterpret_cast<std::int16_t *>(complete_address(0x16));
            completion.found = reinterpret_cast<const char ***>(complete_address(0x1C));
            completion.typed = reinterpret_cast<const char **>(complete_address(0x21));
            buh_tab_completion_count = completion.count;
            buh_tab_completion_continue = complete + 0x5C + 7;
            write_jump(complete + 0x5C, reinterpret_cast<const void *>(buh_tab_completion_asm), 7);
        }
    }
}
