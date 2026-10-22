// SPDX-License-Identifier: GPL-3.0-only

#include "bink.hpp"
#include "bink_player.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <algorithm>
#include <string>

// Longer frames than this (a hitch, or the window being away) don't move the video any further
#define MAXIMUM_FRAME_SECONDS 0.25

// How long the video has to have stopped running before it's paused, so a frame or two without the HUD doesn't
// restart the sound
#define PAUSE_DELAY_SECONDS 0.1

namespace Buh {
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

    static void stop_video() noexcept {
        player.close();
        if(video.pausing_game) {
            set_game_paused(false);
            video.pausing_game = false;
        }
    }

    static void draw(IDirect3DDevice9 *device, double) noexcept {
        if(player.is_open()) {
            player.draw(device, video.fullscreen, video.x, video.y, video.width, video.height);
            video.drawn = true;
        }
    }

    // Once a frame: the video's clock only runs while it's being drawn and, in singleplayer, while the game
    // isn't paused (unless the video paused it) and the console is closed
    static void present(IDirect3DDevice9 *device) noexcept {
        if(!player.is_open()) {
            return;
        }
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        double seconds = std::min(static_cast<double>(now.QuadPart - video.last_present.QuadPart) / frequency.QuadPart, MAXIMUM_FRAME_SECONDS);
        video.last_present = now;

        // Closing the pause menu unpauses the game, so a video that pauses it keeps doing so
        if(video.pausing_game) {
            set_game_paused(true);
        }
        bool running = video.drawn && (!singleplayer() || ((video.pausing_game || !game_paused()) && !console_open()));
        video.stopped_seconds = running ? 0.0 : video.stopped_seconds + seconds;
        bool paused = video.stopped_seconds >= PAUSE_DELAY_SECONDS;
        player.set_paused(paused);
        if(!player.update(device, paused ? 0.0 : seconds)) {
            stop_video();
        }
        video.drawn = false;
    }

    static void reset() noexcept {
        player.release_surface();
    }

    struct VideoOptions {
        bool loop = false;
        bool pause = false;
    };

    static void start_video(const std::string &path, bool fullscreen, VideoOptions options, float x, float y, float width, float height) noexcept {
        if(!hud_drawing_available() || !halo_device()) {
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

    void set_up_bink() noexcept {
        if(!hud_drawing_available()) {
            return;
        }
        add_hud_draw_callback(draw);
        add_present_callback(present);
        add_reset_callback(reset);

        add_console_command("buh_bink_play", "<file> [loop] [pause]", 1, 3, play_command);
        add_console_command("buh_bink_pip", "<file> <x> <y> <width> <height> [loop] [pause]", 5, 7, pip_command);
        add_console_command("buh_bink_stop", "", 0, 0, stop_command);
        add_console_command("buh_bink_volume", "[volume]", 0, 1, volume_command);
    }
}
