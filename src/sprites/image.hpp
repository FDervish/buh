// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_SPRITES_IMAGE_HPP
#define BUH_SPRITES_IMAGE_HPP

#include <windows.h>
#include <d3d9.h>
#include <cstddef>

namespace Buh {
    /**
     * Load a PNG, JPEG, BMP, GIF or TIFF (anything Windows can decode) into a managed texture
     * @return the texture, or nullptr if it couldn't be loaded
     */
    IDirect3DTexture9 *load_image(IDirect3DDevice9 *device, const wchar_t *path, UINT &width, UINT &height) noexcept;

    /**
     * Load an image file held in memory, like one of buh's resources. A gray one can go in an 8 bit luminance texture
     * (a quarter of the memory) where the device has them; its red, green and blue all read the same either way.
     */
    IDirect3DTexture9 *load_image(IDirect3DDevice9 *device, const void *data, std::size_t size, bool gray, UINT &width, UINT &height) noexcept;

    /**
     * A corner of an image on screen: where it goes, in pixels, and which part of the image it shows, from 0 to 1
     */
    struct ImageCorner {
        float x;
        float y;
        float u;
        float v;
    };

    /**
     * Set the states draw_image needs. Capture the device's own first (with a state block) and put them back after.
     */
    void set_image_states(IDirect3DDevice9 *device) noexcept;

    /**
     * Draw a texture as a quad, corners clockwise from the top left, faded by alpha from 0 to 1
     */
    void draw_image(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const ImageCorner (&corners)[4], float alpha) noexcept;
}

#endif
