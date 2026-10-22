// SPDX-License-Identifier: GPL-3.0-only
//
// The idea comes from Harmony's audio engine, which also uses SoLoud.

#include "audio.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <soloud.h>
#include <soloud_wavstream.h>

#include <map>
#include <memory>
#include <string>

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

    static bool start_engine() noexcept {
        if(engine) {
            return true;
        }
        auto *starting = new SoLoud::Soloud;
        if(starting->init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::WASAPI) != SoLoud::SO_NO_ERROR) {
            delete starting;
            return false;
        }
        engine = starting;
        engine->setGlobalVolume(volume);
        return true;
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
            console_error("buh: couldn't start audio");
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
