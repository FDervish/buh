// SPDX-License-Identifier: GPL-3.0-only

#include "subtitles.hpp"
#include "sound_length.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../map/map_load.hpp"
#include "../memory.hpp"
#include "../render.hpp"
#include "../text/text.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>

#define PERMUTATION_SUBTITLE_DATA_OFFSET 0x68

#define FADE_IN_SECONDS 0.12
#define FADE_OUT_SECONDS 0.25
#define DEFAULT_SECONDS 4.0
#define MAXIMUM_SUBTITLES 3

// In the HUD's 480 unit tall canvas
#define SUBTITLE_BOTTOM 440
#define SUBTITLE_WIDTH 480
#define SUBTITLE_SPACING 4

namespace Buh {
    struct Subtitle {
        std::wstring text;
        int lines;
        double age;
        double seconds;
        const void *source;
    };

    static std::deque<Subtitle> subtitles;
    static bool enabled = true;

    extern "C" {
        const void *buh_sound_enqueue_continue = nullptr;
        void buh_sound_enqueue_asm() noexcept;
    }

    // Wrap to the subtitle width at spaces, joining lines with Halo's |n
    static std::wstring wrap(const std::wstring &text, std::uint32_t font, int &lines) noexcept {
        std::wstring wrapped, line, word;
        lines = 0;
        auto end_line = [&]() {
            wrapped += (lines++ ? L"|n" : L"") + line;
            line.clear();
        };
        auto add_word = [&]() {
            if(word.empty()) {
                return;
            }
            auto candidate = line.empty() ? word : line + L' ' + word;
            if(!line.empty() && text_width(candidate, font) > SUBTITLE_WIDTH) {
                end_line();
                line = word;
            }
            else {
                line = std::move(candidate);
            }
            word.clear();
        };
        for(std::size_t i = 0; i < text.size(); i++) {
            if(text[i] == L'\n' || (text[i] == L'|' && i + 1 < text.size() && text[i + 1] == L'n')) {
                add_word();
                end_line();
                i += text[i] == L'|';
            }
            else if(text[i] == L' ' || text[i] == L'\r') {
                add_word();
            }
            else {
                word += text[i];
            }
        }
        add_word();
        if(!line.empty() || lines == 0) {
            end_line();
        }
        return wrapped;
    }

    static void add_subtitle(const std::wstring &text, double seconds, const void *source) noexcept {
        for(auto &subtitle : subtitles) {
            if(source && subtitle.source == source) {
                return;
            }
        }
        subtitles.push_back({ text, 0, 0.0, std::max(seconds, FADE_IN_SECONDS + FADE_OUT_SECONDS), source });
        while(subtitles.size() > MAXIMUM_SUBTITLES) {
            subtitles.pop_front();
        }
    }

    // A permutation's subtitles, if it has any
    static const wchar_t *subtitle_data(const std::byte *permutation, std::uint32_t &size) noexcept {
        size = *reinterpret_cast<const std::uint32_t *>(permutation + PERMUTATION_SUBTITLE_DATA_OFFSET);
        auto *data = *reinterpret_cast<const wchar_t * const *>(permutation + PERMUTATION_SUBTITLE_DATA_OFFSET + 0xC);
        return size >= sizeof(wchar_t) && readable(data, size) ? data : nullptr;
    }

    static bool has_subtitles(const std::byte *permutation) noexcept {
        std::uint32_t size;
        return subtitle_data(permutation, size) != nullptr;
    }

    extern "C" void buh_sound_enqueued(const std::byte *permutation) noexcept {
        if(!enabled || !permutation) {
            return;
        }
        std::uint32_t size;
        auto *data = subtitle_data(permutation, size);
        auto *sound = sound_of_permutation(permutation);
        if(!data || !sound) {
            return;
        }
        std::wstring text(data, std::find(data, data + size / sizeof(wchar_t), L'\0'));

        // A long sound plays as a chain of permutations. Its subtitles stay up through the ones after it that have
        // none of their own, so text on the first covers all of it, and text on a later one takes over from there.
        auto seconds = sound_chain_seconds(sound, permutation, has_subtitles);
        add_subtitle(text, seconds > 0.0 ? seconds : DEFAULT_SECONDS, permutation);
    }

    static void draw(IDirect3DDevice9 *, double seconds) noexcept {
        if(subtitles.empty()) {
            return;
        }
        auto font = hud_message_font();
        auto line_height = font_line_height(font);
        if(line_height <= 0) {
            return;
        }
        int width = static_cast<int>(hud_canvas_width());
        int bottom = SUBTITLE_BOTTOM;

        // Newest at the bottom, pushing older ones up
        for(auto subtitle = subtitles.rbegin(); subtitle != subtitles.rend(); subtitle++) {
            if(subtitle->lines == 0) {
                subtitle->text = wrap(subtitle->text, font, subtitle->lines);
            }
            auto alpha = std::clamp(std::min(subtitle->age / FADE_IN_SECONDS, (subtitle->seconds - subtitle->age) / FADE_OUT_SECONDS), 0.0, 1.0);
            int top = bottom - subtitle->lines * line_height;
            const float shadow[4] = { static_cast<float>(alpha * 0.75), 0.0F, 0.0F, 0.0F };
            const float color[4] = { static_cast<float>(alpha), 1.0F, 1.0F, 1.0F };
            draw_text(subtitle->text, 1, top + 1, width + 1, bottom + 1, font, TEXT_ALIGN_CENTER, shadow);
            draw_text(subtitle->text, 0, top, width, bottom, font, TEXT_ALIGN_CENTER, color);
            bottom = top - SUBTITLE_SPACING;
        }

        for(auto &subtitle : subtitles) {
            subtitle.age += seconds;
        }
        subtitles.erase(std::remove_if(subtitles.begin(), subtitles.end(), [](const Subtitle &subtitle) {
            return subtitle.age >= subtitle.seconds;
        }), subtitles.end());
    }

    static void subtitles_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_subtitles [true|false]");
            return;
        }
        console_print(enabled ? "buh_subtitles: true" : "buh_subtitles: false");
    }

    static void subtitle_command(const std::vector<std::string> &arguments) {
        float seconds = static_cast<float>(DEFAULT_SECONDS);
        if(arguments.size() > 1 && (!parse_float(arguments[1], seconds) || seconds <= 0.0F)) {
            console_error("usage: buh_subtitle <text> [seconds]");
            return;
        }
        auto length = MultiByteToWideChar(CP_UTF8, 0, arguments[0].c_str(), -1, nullptr, 0);
        std::wstring text(length > 0 ? length - 1 : 0, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, arguments[0].c_str(), -1, text.data(), length);
        add_subtitle(text, seconds, nullptr);
    }

    void set_up_subtitles() noexcept {
        if(!hud_drawing_available() || !text_available()) {
            return;
        }
        // Where Halo queues a sound permutation (in edi) on a channel: movsx eax, cx; push esi; lea esi, [eax + eax * 2]
        auto *enqueue = find_signature({ 0x0F, 0xBF, 0xC1, 0x56, 0x8D, 0x34, 0x40, 0x8B, 0x04, 0xF5, -1, -1, -1, -1, 0x85, 0xC0 });
        if(enqueue) {
            buh_sound_enqueue_continue = enqueue + 7;
            write_jump(enqueue, reinterpret_cast<const void *>(buh_sound_enqueue_asm), 7);
            add_console_command("buh_subtitles", "[true|false]", 0, 1, subtitles_command);
        }
        add_hud_draw_callback(draw);
        add_before_map_load_callback([](const char *) noexcept { subtitles.clear(); });
        add_console_command("buh_subtitle", "<text> [seconds]", 1, 2, subtitle_command);
    }
}
