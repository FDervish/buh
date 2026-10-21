// SPDX-License-Identifier: GPL-3.0-only

#include "bink.hpp"
#include "bink_player.hpp"
#include "../console.hpp"
#include "../memory.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#define DEVICE_RESET_INDEX 16
#define DEVICE_PRESENT_INDEX 17

// Longer frames than this (a hitch, or the window being away) don't move the video any further
#define MAXIMUM_FRAME_SECONDS 0.25

// How long the video has to have stopped running before it's paused, so a frame or two without the HUD doesn't
// restart the sound
#define PAUSE_DELAY_SECONDS 0.1

namespace Buh {
    using PresentFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty);
    using ResetFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters);
    static PresentFunction original_present = nullptr;
    static ResetFunction original_reset = nullptr;

    // Halo's globals
    static IDirect3DDevice9 **halo_device = nullptr;
    static std::uint8_t **game_time_globals = nullptr;
    static const std::uint8_t *console_open = nullptr;
    static const std::int16_t *server_type = nullptr;

    extern "C" {
        const void *buh_interface_draw = nullptr;
        void buh_interface_draw_asm() noexcept;
    }

    static BinkPlayer player;
    static struct {
        bool fullscreen = false;
        bool pausing_game = false;
        bool drawn = false;
        double stopped_seconds = 0.0;
        float x = 0.0F;
        float y = 0.0F;
        float width = 1.0F;
        float height = 1.0F;
        LARGE_INTEGER last_present = {};
    } video;

    static bool singleplayer() noexcept {
        return !server_type || *server_type == 0;
    }

    // game_time_set_paused(), which is what the pause menu calls
    static void set_game_paused(bool paused) noexcept {
        auto *globals = game_time_globals ? *game_time_globals : nullptr;
        if(!globals) {
            return;
        }
        if(globals[0]) {
            globals[1] = !paused;
        }
        globals[2] = paused;
    }

    static bool game_paused() noexcept {
        auto *globals = game_time_globals ? *game_time_globals : nullptr;
        return globals && globals[2];
    }

    static void stop_video() noexcept {
        player.close();
        if(video.pausing_game) {
            set_game_paused(false);
            video.pausing_game = false;
        }
    }

    // Runs right after Halo draws the HUD, so the video sorts with it: menus and the console go on top, and
    // nothing is drawn when Halo isn't drawing the HUD
    extern "C" void buh_after_interface_draw() noexcept {
        if(player.is_open() && halo_device && *halo_device) {
            player.draw(*halo_device, video.fullscreen, video.x, video.y, video.width, video.height);
            video.drawn = true;
        }
    }

    // Once a frame: the video's clock only runs while it's being drawn and, in singleplayer, while the game
    // isn't paused (unless the video paused it) and the console is closed
    static HRESULT __stdcall present(IDirect3DDevice9 *device, const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty) {
        if(player.is_open()) {
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            double seconds = std::min(static_cast<double>(now.QuadPart - video.last_present.QuadPart) / frequency.QuadPart, MAXIMUM_FRAME_SECONDS);
            video.last_present = now;

            // Closing the pause menu unpauses the game, so a video that pauses it keeps doing so
            if(video.pausing_game) {
                set_game_paused(true);
            }
            bool running = video.drawn && (!singleplayer() || ((video.pausing_game || !game_paused()) && !(console_open && *console_open)));
            video.stopped_seconds = running ? 0.0 : video.stopped_seconds + seconds;
            bool paused = video.stopped_seconds >= PAUSE_DELAY_SECONDS;
            player.set_paused(paused);
            if(!player.update(device, paused ? 0.0 : seconds)) {
                stop_video();
            }
            video.drawn = false;
        }
        return original_present(device, source, destination, window, dirty);
    }

    static HRESULT __stdcall reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters) {
        player.release_surface();
        return original_reset(device, parameters);
    }

    static bool hook_device() noexcept {
        if(original_present) {
            return true;
        }
        auto *device = halo_device ? *halo_device : nullptr;
        if(!device) {
            return false;
        }
        auto **vtable = *reinterpret_cast<void ***>(device);
        original_present = reinterpret_cast<PresentFunction>(vtable[DEVICE_PRESENT_INDEX]);
        original_reset = reinterpret_cast<ResetFunction>(vtable[DEVICE_RESET_INDEX]);
        overwrite(vtable + DEVICE_PRESENT_INDEX, reinterpret_cast<void *>(present));
        overwrite(vtable + DEVICE_RESET_INDEX, reinterpret_cast<void *>(reset));
        return true;
    }

    struct VideoOptions {
        bool loop = false;
        bool pause = false;
    };

    static void start_video(const std::string &path, bool fullscreen, VideoOptions options, float x, float y, float width, float height) noexcept {
        if(!buh_interface_draw || !hook_device()) {
            console_error("buh: no Direct3D device yet");
            return;
        }

        stop_video();
        auto file = path;
        if(file.find('.') == std::string::npos) {
            file += ".bik";
        }
        if(!player.open(file.c_str(), options.loop)) {
            console_error(("buh: couldn't open " + file).c_str());
            return;
        }
        if(options.pause && !singleplayer()) {
            console_error("buh: pause only works in singleplayer, so the game keeps running");
            options.pause = false;
        }
        video.fullscreen = fullscreen;
        video.pausing_game = options.pause;
        video.drawn = false;
        video.stopped_seconds = 0.0;
        video.x = x;
        video.y = y;
        video.width = width;
        video.height = height;
        QueryPerformanceCounter(&video.last_present);
    }

    static bool parse_float(const std::string &text, float &value) noexcept {
        char *end;
        value = std::strtof(text.c_str(), &end);
        return end != text.c_str() && *end == '\0';
    }

    // Optional flags after the required arguments, in any order
    static bool parse_options(const std::vector<std::string> &arguments, std::size_t first, VideoOptions &options) noexcept {
        for(auto i = first; i < arguments.size(); i++) {
            if(arguments[i] == "loop") {
                options.loop = true;
            }
            else if(arguments[i] == "pause") {
                options.pause = true;
            }
            else {
                return false;
            }
        }
        return true;
    }

    static void play_command(const std::vector<std::string> &arguments) {
        VideoOptions options;
        if(!parse_options(arguments, 1, options)) {
            console_error("usage: buh_bink_play <file> [loop] [pause]");
            return;
        }
        start_video(arguments[0], true, options, 0.0F, 0.0F, 1.0F, 1.0F);
    }

    static void pip_command(const std::vector<std::string> &arguments) {
        float x, y, width, height;
        VideoOptions options;
        if(!parse_float(arguments[1], x) || !parse_float(arguments[2], y) || !parse_float(arguments[3], width) || !parse_float(arguments[4], height)
        || width <= 0.0F || height <= 0.0F || !parse_options(arguments, 5, options)) {
            console_error("usage: buh_bink_pip <file> <x> <y> <width> <height> [loop] [pause], as fractions of the screen");
            return;
        }
        start_video(arguments[0], false, options, x, y, width, height);
    }

    static void stop_command(const std::vector<std::string> &) {
        stop_video();
    }

    static float volume = 1.0F;

    static void volume_command(const std::vector<std::string> &arguments) {
        float value;
        if(!arguments.empty()) {
            if(!parse_float(arguments[0], value) || value < 0.0F) {
                console_error("usage: buh_bink_volume [volume], where 1 is normal");
                return;
            }
            volume = value;
            player.set_volume(volume);
        }
        console_print(("buh_bink_volume: " + std::to_string(volume)).c_str());
    }

    template<typename T> static T *read_address(const std::byte *at) noexcept {
        std::uint32_t address;
        std::memcpy(&address, at, sizeof(address));
        return reinterpret_cast<T *>(address);
    }

    void set_up_bink() noexcept {
        // Halo's own player, which reads the device from a global
        auto *halo_player = find_signature({ 0x83, 0xEC, 0x68, 0xA1, -1, -1, -1, -1, 0x53, 0x33, 0xDB, 0x3B, 0xC3, 0x89, 0x5C, 0x24, 0x0C, 0x89, 0x5C, 0x24, 0x08, 0x88, 0x5C, 0x24, 0x05, 0xC6, 0x44, 0x24, 0x06, 0x01, 0x88, 0x5C, 0x24, 0x07, 0x0F, 0x85, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x8B, 0x08, 0x53, 0x8D, 0x54, 0x24, 0x0C, 0x52, 0x53, 0x6A, 0x16 });
        // The end of render_window: lens flares, then the HUD (interface_draw_screen), then the screen flash, then menus
        auto *hud_calls = find_signature({ 0xE8, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x8B, 0xC5, 0xE8, -1, -1, -1, -1, 0x66, 0x83, 0x3D, -1, -1, -1, -1, 0xFF, 0x75 });
        auto *time_globals = find_signature({ 0x8B, 0x15, -1, -1, -1, -1, 0x8A, 0x42, 0x02, 0x84, 0xC0, 0x75, 0x22, 0x8B, 0x0D });
        auto *console = find_signature({ 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x28, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x75, 0x1F, 0x57 });
        auto *server = find_signature({ 0x0F, 0xBF, 0x2D, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x39, 0x1D, -1, -1, -1, -1, 0x75, 0x05 });
        if(!halo_player || !hud_calls || !time_globals || !console || !server) {
            return;
        }
        halo_device = read_address<IDirect3DDevice9 *>(halo_player + 0x29);
        game_time_globals = read_address<std::uint8_t *>(time_globals + 2);
        console_open = read_address<const std::uint8_t>(console + 1) - 1;
        server_type = read_address<const std::int16_t>(server + 3);

        auto *hud_call = hud_calls + 5;
        std::int32_t relative;
        std::memcpy(&relative, hud_call + 1, sizeof(relative));
        buh_interface_draw = hud_call + 5 + relative;
        retarget_call(hud_call, reinterpret_cast<const void *>(buh_interface_draw_asm));

        add_console_command("buh_bink_play", "<file> [loop] [pause]", 1, 3, play_command);
        add_console_command("buh_bink_pip", "<file> <x> <y> <width> <height> [loop] [pause]", 5, 7, pip_command);
        add_console_command("buh_bink_stop", "", 0, 0, stop_command);
        add_console_command("buh_bink_volume", "[volume]", 0, 1, volume_command);
    }
}
