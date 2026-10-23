// SPDX-License-Identifier: GPL-3.0-only
//
// Xbox Halo's loading screen tone: a short hum looped in four voices, pitched to a root, a fifth, an octave and a ninth,
// which come in one after another and fade out as the loading screen goes on.

#include "loading_tone.hpp"
#include "../audio/audio.hpp"
#include "../console.hpp"
#include "../memory.hpp"

#include <windows.h>
#include <soloud.h>
#include <soloud_wav.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#define TONE_SAMPLE_RATE 22050.0F
#define ADPCM_BLOCK_SIZE 36
#define VOICES 4

// The rate the Xbox's voice pitches are relative to, whatever the sound's own
#define XBOX_PITCH_RATE 48000.0

// How long the tone takes to fade out once the loading screen is gone
#define STOP_SECONDS 0.25

// Longer gaps than this between updates (the window being dragged, say) don't move it on any further
#define MAXIMUM_UPDATE_SECONDS 0.25

namespace Buh {
    // As the Xbox has them: each voice's pitch in 1/4096 of an octave from 48 kHz, the part of the loading screen it
    // sounds in, and how far it rises above -50 dB, in hundredths of a decibel
    static const double PITCHES[VOICES] = { -4681.0, -6386.0, -3999.0, -8773.0 };
    static const double RANGES[VOICES][2] = { { 0.0, 1.0 }, { 0.4, 1.0 }, { 0.5, 1.0 }, { 0.55, 1.0 } };
    static const double PEAKS[VOICES] = { 3500.0, 4500.0, 3500.0, 4500.0 };
    static const double SILENT = -5000.0;

    static const std::int16_t ADPCM_STEPS[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767
    };
    static const std::int8_t ADPCM_STEP_CHANGES[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

    static float volume = 1.0F;

    // Never freed, like the rest of buh's sound
    static SoLoud::Wav *tone = nullptr;
    static bool tone_unavailable = false;
    static SoLoud::handle voices[VOICES] = {};
    static bool playing = false;
    static double heard_progress = 0.0;
    static double wobble = 0.0;
    static LARGE_INTEGER last_update = {};

    // Xbox ADPCM, mono: each 36 byte block is its first sample and a step index, then 64 four bit steps, low ones first
    static std::vector<std::int16_t> decode_xbox_adpcm(const std::byte *data, std::size_t size) {
        std::vector<std::int16_t> samples;
        samples.reserve(size / ADPCM_BLOCK_SIZE * 65);
        for(std::size_t block = 0; block + ADPCM_BLOCK_SIZE <= size; block += ADPCM_BLOCK_SIZE) {
            int sample = static_cast<std::int16_t>(static_cast<std::uint16_t>(data[block]) | static_cast<std::uint16_t>(data[block + 1]) << 8);
            int index = std::min(static_cast<int>(data[block + 2]), 88);
            samples.push_back(static_cast<std::int16_t>(sample));
            for(std::size_t i = 4; i < ADPCM_BLOCK_SIZE; i++) {
                auto byte = static_cast<unsigned>(data[block + i]);
                for(auto nibble : { byte & 0xF, byte >> 4 }) {
                    int step = ADPCM_STEPS[index];
                    int difference = step >> 3;
                    if(nibble & 4) {
                        difference += step;
                    }
                    if(nibble & 2) {
                        difference += step >> 1;
                    }
                    if(nibble & 1) {
                        difference += step >> 2;
                    }
                    sample = std::clamp(nibble & 8 ? sample - difference : sample + difference, -32768, 32767);
                    index = std::clamp(index + ADPCM_STEP_CHANGES[nibble & 7], 0, 88);
                    samples.push_back(static_cast<std::int16_t>(sample));
                }
            }
        }
        return samples;
    }

    static bool load_tone() noexcept {
        if(tone || tone_unavailable) {
            return tone != nullptr;
        }
        std::size_t size;
        auto *data = buh_resource("LOADING_SCREEN_TONE", size);
        auto samples = data ? decode_xbox_adpcm(data, size) : std::vector<std::int16_t>();
        auto *loaded = new SoLoud::Wav;
        if(samples.empty() || loaded->loadRawWave16(reinterpret_cast<short *>(samples.data()), static_cast<unsigned>(samples.size()), TONE_SAMPLE_RATE, 1) != SoLoud::SO_NO_ERROR) {
            delete loaded;
            tone_unavailable = true;
            return false;
        }
        tone = loaded;
        return true;
    }

    // A voice's volume where it's heard to be through the loading screen, as the Xbox works it out
    static float voice_gain(int voice, double progress) noexcept {
        auto from = RANGES[voice][0], to = RANGES[voice][1];
        double t = progress < from || progress > to ? 0.0 : (progress - from) / (to - from);
        double hundredths = std::pow(std::max(0.0, std::sin(t * 3.1415)), 0.3) * PEAKS[voice] + SILENT;
        return static_cast<float>(std::pow(10.0, hundredths / 2000.0));
    }

    // How fast a voice plays the tone, from its pitch on the Xbox
    static float voice_speed(int voice) noexcept {
        return static_cast<float>(XBOX_PITCH_RATE * std::pow(2.0, PITCHES[voice] / 4096.0) / TONE_SAMPLE_RATE);
    }

    // The Xbox moves what's heard a fifth of the way to the progress each frame, 30 times a second, with a slight wobble
    static void move_heard_progress(double progress, double seconds) noexcept {
        auto frames = seconds * 30.0;
        wobble += 0.01 * frames;
        heard_progress += (std::sin(wobble) * 0.05 + progress - heard_progress) * (1.0 - std::pow(0.8, frames));
        heard_progress = std::min(heard_progress, 1.0);
    }

    void update_loading_tone(double progress) noexcept {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        auto seconds = playing ? std::min(static_cast<double>(now.QuadPart - last_update.QuadPart) / frequency.QuadPart, MAXIMUM_UPDATE_SECONDS) : 0.0;
        last_update = now;
        if(volume <= 0.0F) {
            stop_loading_tone();
            return;
        }
        auto *engine = audio_engine();
        if(!engine || !load_tone()) {
            return;
        }
        if(!playing) {
            heard_progress = 0.0;
            wobble = 0.0;
            for(int v = 0; v < VOICES; v++) {
                voices[v] = engine->play(*tone, 0.0F, 0.0F, true);
                engine->setLooping(voices[v], true);
                engine->setRelativePlaySpeed(voices[v], voice_speed(v));
                // Mono at full volume in both speakers, as on the Xbox, rather than 3 dB down in each
                engine->setPanAbsolute(voices[v], 1.0F, 1.0F);
            }
            playing = true;
        }
        move_heard_progress(std::clamp(progress, 0.0, 1.0), seconds);
        for(int v = 0; v < VOICES; v++) {
            engine->setVolume(voices[v], voice_gain(v, heard_progress) * volume);
            // It goes on while the game's paused, which pauses the rest of buh's sound
            engine->setPause(voices[v], false);
        }
    }

    void stop_loading_tone() noexcept {
        if(!playing) {
            return;
        }
        auto *engine = audio_engine();
        for(auto voice : voices) {
            engine->fadeVolume(voice, 0.0F, STOP_SECONDS);
            engine->scheduleStop(voice, STOP_SECONDS);
        }
        playing = false;
    }

    static void tone_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty()) {
            float value;
            if(!parse_float(arguments[0], value) || value < 0.0F) {
                console_error("usage: buh_loading_screen_tone [volume], where 1 is as loud as on the Xbox and 0 is off");
                return;
            }
            volume = value;
        }
        console_print(("buh_loading_screen_tone: " + std::to_string(volume)).c_str());
    }

    void set_up_loading_tone() noexcept {
        add_console_command("buh_loading_screen_tone", "[volume]", 0, 1, tone_command);
    }
}
