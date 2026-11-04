// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_POST_POST_HPP
#define BUH_POST_POST_HPP

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

namespace Buh {
    /**
     * A texture a post process draws into, and its surface
     */
    struct PostTarget {
        IDirect3DTexture9 *texture;
        IDirect3DSurface9 *surface;
        UINT width;
        UINT height;
    };

    /**
     * Make a render target texture in the default pool, which a device reset mustn't find
     * @return false if it couldn't, with whatever was made of it released
     */
    bool create_post_target(IDirect3DDevice9 *device, PostTarget &target, UINT width, UINT height, D3DFORMAT format) noexcept;
    void release_post_target(PostTarget &target) noexcept;

    /**
     * What a post process works with in a world draw: Halo's render target, its format and depth buffer, the viewport
     * the world was drawn in, and the world as it is so far, copied out of the viewport in that format
     */
    struct PostFrame {
        IDirect3DSurface9 *target;
        D3DFORMAT format;
        IDirect3DSurface9 *depth;
        D3DVIEWPORT9 viewport;
        const PostTarget *world;
    };

    /**
     * Whether the device's adapter can have a resource of this format for this usage (D3DUSAGE_QUERY_FILTER, say)
     */
    bool post_format_supported(IDirect3DDevice9 *device, DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format) noexcept;

    /**
     * Copy the world out and set the states post processes draw with, keeping Halo's to put back. Post processes go one
     * after another, each starting from what the last left, as the world callbacks they're drawn from do.
     * @return false if it can't, and then there's nothing to end
     */
    bool begin_post_frame(IDirect3DDevice9 *device, PostFrame &frame) noexcept;

    /**
     * Put back Halo's render target, depth buffer and states
     */
    void end_post_frame(IDirect3DDevice9 *device, PostFrame &frame) noexcept;

    /**
     * Draw the pixel shader set all over a target, or all over the world in Halo's render target
     */
    void draw_post_pass(IDirect3DDevice9 *device, const PostTarget &into) noexcept;
    void draw_post_world(IDirect3DDevice9 *device, const PostFrame &frame) noexcept;

    /**
     * Whether the device has pixel shader 3.0, which post processes are written for, and make them
     * @return false if it hasn't or they couldn't be made, with any that were released
     */
    bool create_post_shaders(IDirect3DDevice9 *device, const DWORD *const *code, IDirect3DPixelShader9 **shaders, int count) noexcept;

    /**
     * A setting from least to most: set it if it's given, then say what it is
     */
    void post_setting_command(const char *name, const char *usage, float &value, float least, float most, const std::vector<std::string> &arguments);

    /**
     * Something that keeps a post process from drawing. A frame or two of it is normal as one starts, so it's only said
     * once it's gone on a while, and then not again until it's reset ({}), as its effect is turned on again. Drawing
     * sets times back to 0.
     */
    struct PostTrouble {
        int times;
        bool told;
    };

    /**
     * It kept a post process from drawing again: say so if it's gone on a while and hasn't been said
     */
    void post_trouble(PostTrouble &trouble, const char *message) noexcept;

    /**
     * 0.05 rather than 0.050000
     */
    std::string post_number_text(float value);

    void set_up_post() noexcept;
}

#endif
