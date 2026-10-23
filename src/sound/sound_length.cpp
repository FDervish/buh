// SPDX-License-Identifier: GPL-3.0-only

#include "sound_length.hpp"
#include "../console.hpp"
#include "../halo.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define SOUND_SAMPLE_RATE_OFFSET 0x06
#define SOUND_CHANNEL_COUNT_OFFSET 0x6C
#define SOUND_PITCH_RANGES_OFFSET 0x98
#define PITCH_RANGE_SIZE 0x48
#define PITCH_RANGE_ACTUAL_PERMUTATION_COUNT_OFFSET 0x2C
#define PITCH_RANGE_PERMUTATIONS_OFFSET 0x3C
#define PERMUTATION_SIZE 0x7C
#define PERMUTATION_FORMAT_OFFSET 0x28
#define PERMUTATION_NEXT_OFFSET 0x2A
#define PERMUTATION_SAMPLES_POINTER_OFFSET 0x30
#define PERMUTATION_SOUND_TAG_OFFSET 0x34
#define PERMUTATION_BUFFER_SIZE_OFFSET 0x38
#define PERMUTATION_SAMPLES_SIZE_OFFSET 0x40

#define SOUND_FORMAT_16_BIT_PCM 0
#define SOUND_FORMAT_XBOX_ADPCM 1
#define SOUND_FORMAT_IMA_ADPCM 2
#define SOUND_FORMAT_OGG_VORBIS 3

// Xbox ADPCM packs 64 samples a channel into 36 byte blocks
#define ADPCM_BLOCK_SIZE 36
#define ADPCM_BLOCK_SAMPLES 64

namespace Buh {
    template<typename T> static T read(const std::byte *at) noexcept {
        T value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }

    // The last Ogg page's granule position is the stream's length in samples
    static std::uint64_t ogg_samples(const std::byte *data, std::size_t size) noexcept {
        for(std::size_t i = size >= 27 ? size - 27 + 1 : 0; i-- > 0;) {
            if(std::memcmp(data + i, "OggS", 4) == 0) {
                auto granule = read<std::uint64_t>(data + i + 6);
                if(granule != UINT64_MAX) {
                    return granule;
                }
            }
        }
        return 0;
    }

    double sound_permutation_seconds(const std::byte *sound, const std::byte *permutation) noexcept {
        double rate = read<std::int16_t>(sound + SOUND_SAMPLE_RATE_OFFSET) == 1 ? 44100.0 : 22050.0;
        double channels = read<std::int16_t>(sound + SOUND_CHANNEL_COUNT_OFFSET) == 1 ? 2.0 : 1.0;
        auto size = read<std::uint32_t>(permutation + PERMUTATION_SAMPLES_SIZE_OFFSET);
        auto buffer_size = read<std::uint32_t>(permutation + PERMUTATION_BUFFER_SIZE_OFFSET);
        switch(read<std::int16_t>(permutation + PERMUTATION_FORMAT_OFFSET)) {
            case SOUND_FORMAT_16_BIT_PCM:
                return size / (2.0 * channels) / rate;
            case SOUND_FORMAT_XBOX_ADPCM:
            case SOUND_FORMAT_IMA_ADPCM:
                return static_cast<double>(size / static_cast<std::uint32_t>(ADPCM_BLOCK_SIZE * channels)) * ADPCM_BLOCK_SAMPLES / rate;
            case SOUND_FORMAT_OGG_VORBIS: {
                // The buffer is what it decompresses to
                if(buffer_size) {
                    return buffer_size / (2.0 * channels) / rate;
                }
                auto *samples = read<const std::byte *>(permutation + PERMUTATION_SAMPLES_POINTER_OFFSET);
                return samples ? ogg_samples(samples, size) / rate : 0.0;
            }
            default:
                return 0.0;
        }
    }

    double sound_chain_seconds(const std::byte *sound, const std::byte *permutation, bool (*stop)(const std::byte *permutation)) noexcept {
        double seconds = sound_permutation_seconds(sound, permutation);

        // The pitch range it's one of, whose permutations the chain goes through
        auto pitch_range_count = block_count(sound + SOUND_PITCH_RANGES_OFFSET);
        auto *pitch_ranges = block_elements(sound + SOUND_PITCH_RANGES_OFFSET);
        if(!readable(pitch_ranges, pitch_range_count * PITCH_RANGE_SIZE)) {
            return seconds;
        }
        for(std::size_t r = 0; r < pitch_range_count; r++) {
            auto *pitch_range = pitch_ranges + r * PITCH_RANGE_SIZE;
            auto permutation_count = block_count(pitch_range + PITCH_RANGE_PERMUTATIONS_OFFSET);
            auto *permutations = block_elements(pitch_range + PITCH_RANGE_PERMUTATIONS_OFFSET);
            if(permutation < permutations || permutation >= permutations + permutation_count * PERMUTATION_SIZE || !readable(permutations, permutation_count * PERMUTATION_SIZE)) {
                continue;
            }
            auto *piece = permutation;
            for(std::size_t links = 0; links < permutation_count; links++) {
                auto next = read<std::int16_t>(piece + PERMUTATION_NEXT_OFFSET);
                if(next < 0 || static_cast<std::size_t>(next) >= permutation_count) {
                    break;
                }
                piece = permutations + next * PERMUTATION_SIZE;
                if(stop && stop(piece)) {
                    break;
                }
                seconds += sound_permutation_seconds(sound, piece);
            }
            break;
        }
        return seconds;
    }

    const std::byte *sound_of_permutation(const std::byte *permutation) noexcept {
        auto *tag = get_tag(read<std::uint32_t>(permutation + PERMUTATION_SOUND_TAG_OFFSET));
        return tag && tag->primary_class == TAG_CLASS_SOUND ? tag->data : nullptr;
    }

    static void sound_length_command(const std::vector<std::string> &arguments) {
        auto *tag = find_tag(arguments[0].c_str(), TAG_CLASS_SOUND);
        if(!tag) {
            console_error(("buh: no sound tag " + arguments[0] + " in this map").c_str());
            return;
        }
        auto *sound = tag->data;
        auto pitch_range_count = block_count(sound + SOUND_PITCH_RANGES_OFFSET);
        auto *pitch_ranges = block_elements(sound + SOUND_PITCH_RANGES_OFFSET);
        for(std::size_t r = 0; r < pitch_range_count && readable(pitch_ranges, pitch_range_count * PITCH_RANGE_SIZE); r++) {
            auto *pitch_range = pitch_ranges + r * PITCH_RANGE_SIZE;
            auto permutation_count = block_count(pitch_range + PITCH_RANGE_PERMUTATIONS_OFFSET);
            auto *permutations = block_elements(pitch_range + PITCH_RANGE_PERMUTATIONS_OFFSET);
            auto actual_count = read<std::int16_t>(pitch_range + PITCH_RANGE_ACTUAL_PERMUTATION_COUNT_OFFSET);
            if(!readable(permutations, permutation_count * PERMUTATION_SIZE)) {
                continue;
            }
            // The first actual_count permutations are the sound's; long ones carry on in chained permutations
            for(std::size_t p = 0; p < permutation_count && static_cast<std::int16_t>(p) < actual_count; p++) {
                auto seconds = sound_chain_seconds(sound, permutations + p * PERMUTATION_SIZE);
                char line[128];
                std::snprintf(line, sizeof(line), "pitch range %zu, permutation %zu (%.31s): %.2f s", r, p, reinterpret_cast<const char *>(permutations + p * PERMUTATION_SIZE), seconds);
                console_print(line);
            }
        }
    }

    void set_up_sound_length() noexcept {
        add_console_command("buh_sound_length", "<sound tag path>", 1, 1, sound_length_command);
    }
}
