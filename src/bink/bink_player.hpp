// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_BINK_PLAYER_HPP
#define BUH_BINK_PLAYER_HPP

#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace Buh {
    struct BinkHeader;

    /**
     * A Bink video that only moves while it's told it's running
     */
    class BinkPlayer {
    public:
        /**
         * Open a video, relative to the game folder like Halo's own
         * @return false if binkw32.dll or the video couldn't be opened
         */
        bool open(const char *path, bool loop) noexcept;
        void close() noexcept;
        bool is_open() const noexcept;

        /**
         * Decode whatever is due and put it on a surface of the device
         * @param seconds how long the video was running since the last update, 0 while it's paused
         * @return false once a video that doesn't loop has ended
         */
        bool update(IDirect3DDevice9 *device, double seconds) noexcept;

        /**
         * Draw the current frame onto the device's render target, letterboxed on black over the viewport or into
         * a rectangle of it given as fractions
         */
        void draw(IDirect3DDevice9 *device, bool fullscreen, float x, float y, float width, float height) noexcept;

        /**
         * Pause or resume the sound
         */
        void set_paused(bool paused) noexcept;

        /**
         * 1 is normal
         */
        void set_volume(float volume) noexcept;

        /**
         * Default pool surfaces have to be released before the device resets
         */
        void release_surface() noexcept;

    private:
        bool frame_due() noexcept;
        void copy_frame() noexcept;

        BinkHeader *bink = nullptr;
        IDirect3DSurface9 *surface = nullptr;
        IDirect3DDevice9 *surface_device = nullptr;
        std::uint32_t surface_frame = 0;
        std::uint32_t shown_frame = 0;
        double frames_per_second = 30.0;
        double own_time = 0.0;
        double stalled = 0.0;
        float volume = 1.0F;
        bool loop = false;
        bool paused = false;
        bool finished = false;
        bool own_clock = false;
    };
}

#endif
