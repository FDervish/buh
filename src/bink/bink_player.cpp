// SPDX-License-Identifier: GPL-3.0-only

#include "bink_player.hpp"

#include <algorithm>
#include <type_traits>

// BinkCopyToBuffer: 32-bit BGRX, which is D3DFMT_X8R8G8B8, and every pixel rather than just what changed
#define BINK_SURFACE_32 3
#define BINK_COPY_ALL 0x80000000

// Frames decoded in one update at most, for catching up when the game runs slower than the video
#define MAXIMUM_FRAMES_TO_DECODE 4

namespace Buh {
    // The start of a BINK, which hasn't changed across versions
    struct BinkHeader {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t frames;
        std::uint32_t frame;
        std::uint32_t last_frame;
        std::uint32_t frame_rate;
        std::uint32_t frame_rate_divisor;
    };

    // binkw32.dll, which Halo uses for its own videos
    static struct {
        BinkHeader *(__stdcall *open)(const char *path, std::uint32_t flags);
        void (__stdcall *close)(BinkHeader *bink);
        std::int32_t (__stdcall *do_frame)(BinkHeader *bink);
        void (__stdcall *next_frame)(BinkHeader *bink);
        std::int32_t (__stdcall *wait)(BinkHeader *bink);
        std::int32_t (__stdcall *copy_to_buffer)(BinkHeader *bink, void *buffer, std::int32_t pitch, std::uint32_t height, std::uint32_t x, std::uint32_t y, std::uint32_t flags);
        std::int32_t (__stdcall *pause)(BinkHeader *bink, std::int32_t pause);
        void (__stdcall *set_volume)(BinkHeader *bink, std::uint32_t track, std::int32_t volume);
        std::int32_t (__stdcall *set_sound_system)(void *open, std::uint32_t parameter);
        void *open_direct_sound;
    } bink_api;

    static bool load_bink() noexcept {
        if(bink_api.open) {
            return true;
        }
        auto *module = LoadLibraryA("binkw32.dll");
        if(!module) {
            return false;
        }
        auto get = [module](auto &function, const char *name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(reinterpret_cast<void *>(GetProcAddress(module, name)));
            return function != nullptr;
        };
        bool loaded = get(bink_api.close, "_BinkClose@4") && get(bink_api.do_frame, "_BinkDoFrame@4")
            && get(bink_api.next_frame, "_BinkNextFrame@4") && get(bink_api.wait, "_BinkWait@4")
            && get(bink_api.copy_to_buffer, "_BinkCopyToBuffer@28")
            && get(bink_api.pause, "_BinkPause@8")
            && get(bink_api.set_volume, "_BinkSetVolume@12") && get(bink_api.set_sound_system, "_BinkSetSoundSystem@8")
            && get(bink_api.open_direct_sound, "_BinkOpenDirectSound@4") && get(bink_api.open, "_BinkOpen@8");
        if(!loaded) {
            bink_api.open = nullptr;
        }
        return loaded;
    }

    // The FAILED macro is a C-style cast
    static bool failed(HRESULT result) noexcept {
        return result < 0;
    }

    bool BinkPlayer::open(const char *path, bool loop) noexcept {
        close();
        if(!load_bink()) {
            return false;
        }
        bink_api.set_sound_system(bink_api.open_direct_sound, 0);
        bink = bink_api.open(path, 0);
        if(!bink) {
            return false;
        }
        this->loop = loop;
        shown_frame = 0;
        surface_frame = 0;
        paused = false;
        finished = false;
        own_clock = false;
        own_time = 0.0;
        stalled = 0.0;
        frames_per_second = bink->frame_rate && bink->frame_rate_divisor ? static_cast<double>(bink->frame_rate) / bink->frame_rate_divisor : 30.0;
        set_volume(volume);
        return true;
    }

    void BinkPlayer::close() noexcept {
        release_surface();
        if(bink) {
            bink_api.close(bink);
            bink = nullptr;
        }
    }

    bool BinkPlayer::is_open() const noexcept {
        return bink != nullptr;
    }

    void BinkPlayer::release_surface() noexcept {
        if(surface) {
            surface->Release();
            surface = nullptr;
        }
        surface_device = nullptr;
        surface_frame = 0;
    }

    bool BinkPlayer::frame_due() noexcept {
        if(own_clock) {
            return own_time >= (bink->frame - 1) / frames_per_second;
        }
        return !bink_api.wait(bink);
    }

    bool BinkPlayer::update(IDirect3DDevice9 *device, double seconds) noexcept {
        if(!bink) {
            return false;
        }

        // A device that reset or was replaced needs a new surface, and the current frame on it again
        if(surface && surface_device != device) {
            release_surface();
        }
        if(!surface) {
            if(failed(device->CreateOffscreenPlainSurface(bink->width, bink->height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &surface, nullptr))) {
                surface = nullptr;
            }
            else {
                surface_device = device;
            }
        }

        // Bink's clock follows the sound, so it says when frames are due. If it stops, which losing the sound
        // would do, the video carries on by its own clock instead of freezing.
        if(own_clock) {
            own_time += seconds;
        }
        bool decoded = false;
        if(seconds > 0.0) {
            for(int i = 0; i < MAXIMUM_FRAMES_TO_DECODE && frame_due(); i++) {
                if(finished) {
                    return false;
                }
                auto frame = bink->frame;
                bink_api.do_frame(bink);
                shown_frame = frame;
                decoded = true;

                // Copied between BinkDoFrame and BinkNextFrame, the way Halo does. Past the last frame, Bink goes
                // back to the first one and keeps the timing (and the sound) seamless, which is how it loops.
                copy_frame();
                bink_api.next_frame(bink);
                if(frame == bink->frames) {
                    finished = !loop;
                    own_time -= bink->frames / frames_per_second;
                }
            }
            stalled = decoded ? 0.0 : stalled + seconds;
            if(!own_clock && stalled > 0.25 + 3.0 / frames_per_second) {
                own_clock = true;
                own_time = (bink->frame - 1) / frames_per_second;
            }
        }
        if(!decoded && surface_frame != shown_frame) {
            copy_frame();
        }
        return true;
    }

    void BinkPlayer::copy_frame() noexcept {
        D3DLOCKED_RECT locked;
        if(surface && !failed(surface->LockRect(&locked, nullptr, 0))) {
            bink_api.copy_to_buffer(bink, locked.pBits, locked.Pitch, bink->height, 0, 0, BINK_SURFACE_32 | BINK_COPY_ALL);
            surface->UnlockRect();
            surface_frame = shown_frame;
        }
    }

    void BinkPlayer::draw(IDirect3DDevice9 *device, bool fullscreen, float x, float y, float width, float height) noexcept {
        if(!surface || surface_device != device || surface_frame == 0) {
            return;
        }
        IDirect3DSurface9 *target = nullptr;
        if(failed(device->GetRenderTarget(0, &target))) {
            return;
        }
        D3DSURFACE_DESC description;
        D3DVIEWPORT9 viewport;
        target->GetDesc(&description);
        device->GetViewport(&viewport);

        float view_x = static_cast<float>(viewport.X);
        float view_y = static_cast<float>(viewport.Y);
        float view_width = static_cast<float>(viewport.Width);
        float view_height = static_cast<float>(viewport.Height);
        if(fullscreen) {
            RECT view = { static_cast<LONG>(viewport.X), static_cast<LONG>(viewport.Y), static_cast<LONG>(viewport.X + viewport.Width), static_cast<LONG>(viewport.Y + viewport.Height) };
            device->ColorFill(target, &view, 0xFF000000);
            float scale = std::min(view_width / bink->width, view_height / bink->height);
            width = bink->width * scale / view_width;
            height = bink->height * scale / view_height;
            x = (1.0F - width) / 2.0F;
            y = (1.0F - height) / 2.0F;
        }

        // StretchRect refuses a rectangle that leaves the surface
        auto clamp = [](float value, std::uint32_t limit) {
            return static_cast<LONG>(std::clamp(value, 0.0F, static_cast<float>(limit)));
        };
        RECT rect;
        rect.left = clamp(view_x + x * view_width, description.Width);
        rect.top = clamp(view_y + y * view_height, description.Height);
        rect.right = clamp(view_x + (x + width) * view_width, description.Width);
        rect.bottom = clamp(view_y + (y + height) * view_height, description.Height);
        if(rect.right > rect.left && rect.bottom > rect.top) {
            device->StretchRect(surface, nullptr, target, &rect, D3DTEXF_LINEAR);
        }
        target->Release();
    }

    void BinkPlayer::set_paused(bool paused) noexcept {
        if(bink && paused != this->paused) {
            bink_api.pause(bink, paused);
            this->paused = paused;
        }
    }

    void BinkPlayer::set_volume(float volume) noexcept {
        this->volume = volume;
        if(bink) {
            bink_api.set_volume(bink, 0, static_cast<std::int32_t>(volume * 32768.0F));
        }
    }
}
