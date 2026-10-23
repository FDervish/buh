// SPDX-License-Identifier: GPL-3.0-only

#include "audio.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <soloud.h>
#include <soloud_wavstream.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace SoLoud {
    // Why Windows wouldn't open the audio device (buh's change to SoLoud's WASAPI backend keeps it)
    extern unsigned long wasapi_last_error;
}

namespace Buh {
    struct Sound {
        std::unique_ptr<SoLoud::WavStream> source;
        SoLoud::handle handle;
    };

    // Never freed: shutting SoLoud down waits for its thread, which can't happen while Windows is unloading DLLs
    static SoLoud::Soloud *engine = nullptr;
    static auto *sounds = new std::map<std::string, Sound>;
    static float volume = 1.0F;
    static bool paused = false;
    static bool engine_failed = false;

    // Says why if it can't
    static bool start_engine() noexcept {
        if(engine) {
            return true;
        }
        auto *starting = new SoLoud::Soloud;
        SoLoud::wasapi_last_error = 0;
        if(starting->init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::WASAPI) != SoLoud::SO_NO_ERROR) {
            delete starting;
            engine_failed = true;
            char message[96];
            std::snprintf(message, sizeof(message), "buh: no sound, Windows wouldn't open the audio device (error 0x%08lX)", SoLoud::wasapi_last_error);
            console_error(message);
            return false;
        }
        engine = starting;
        engine->setGlobalVolume(volume);
        return true;
    }

    // Once it's failed, only buh_audio_play tries again, not every frame of the loading screen
    SoLoud::Soloud *audio_engine() noexcept {
        return engine || (!engine_failed && start_engine()) ? engine : nullptr;
    }

    static void stop(const std::string &name) noexcept {
        auto sound = sounds->find(name);
        if(sound != sounds->end()) {
            engine->stop(sound->second.handle);
            sounds->erase(sound);
        }
    }

    // Once a frame: pause with the game, and forget sounds that have finished
    static void present(IDirect3DDevice9 *) noexcept {
        if(!engine) {
            return;
        }
        if(paused != game_time_stopped()) {
            paused = !paused;
            engine->setPauseAll(paused);
        }
        for(auto sound = sounds->begin(); sound != sounds->end();) {
            sound = engine->isValidVoiceHandle(sound->second.handle) ? std::next(sound) : sounds->erase(sound);
        }
    }

    static void play_command(const std::vector<std::string> &arguments) {
        bool loop = false;
        float sound_volume = 1.0F;
        for(std::size_t i = 2; i < arguments.size(); i++) {
            if(arguments[i] == "loop") {
                loop = true;
            }
            else if(!parse_float(arguments[i], sound_volume) || sound_volume < 0.0F) {
                console_error("usage: buh_audio_play <name> <file> [loop] [volume]");
                return;
            }
        }
        if(!start_engine()) {
            return;
        }
        auto &name = arguments[0];
        stop(name);
        auto source = std::make_unique<SoLoud::WavStream>();
        if(source->load(arguments[1].c_str()) != SoLoud::SO_NO_ERROR) {
            console_error(("buh: couldn't open " + arguments[1]).c_str());
            return;
        }
        source->setLooping(loop);
        auto handle = engine->play(*source, sound_volume, 0.0F, paused);
        (*sounds)[name] = { std::move(source), handle };
    }

    static void stop_command(const std::vector<std::string> &arguments) {
        if(!engine) {
            return;
        }
        if(arguments.empty()) {
            engine->stopAll();
            sounds->clear();
        }
        else {
            stop(arguments[0]);
        }
    }

    static void volume_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty()) {
            float value;
            if(!parse_float(arguments[0], value) || value < 0.0F) {
                console_error("usage: buh_audio_volume [volume], where 1 is normal");
                return;
            }
            volume = value;
            if(engine) {
                engine->setGlobalVolume(volume);
            }
        }
        console_print(("buh_audio_volume: " + std::to_string(volume)).c_str());
    }

    void set_up_audio() noexcept {
        add_present_callback(present);
        add_console_command("buh_audio_play", "<name> <file> [loop] [volume]", 2, 4, play_command);
        add_console_command("buh_audio_stop", "[name]", 0, 1, stop_command);
        add_console_command("buh_audio_volume", "[volume]", 0, 1, volume_command);
    }
}
