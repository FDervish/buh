// SPDX-License-Identifier: GPL-3.0-only

#include "image.hpp"

#include <wincodec.h>

namespace Buh {
    template<typename T> static void release(T *&object) noexcept {
        if(object) {
            object->Release();
            object = nullptr;
        }
    }

    static IWICImagingFactory *factory() noexcept {
        static IWICImagingFactory *factory = nullptr;
        if(!factory) {
            // Halo's thread already has COM, so this only makes sure
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void **>(&factory)) < 0) {
                factory = nullptr;
            }
        }
        return factory;
    }

    // The decoder's first frame, as luminance if it's gray and the device can, otherwise straight BGRA
    static IDirect3DTexture9 *load_decoded(IDirect3DDevice9 *device, IWICImagingFactory *wic, IWICBitmapDecoder *decoder, bool gray, UINT &width, UINT &height) noexcept {
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter *converter = nullptr;
        IDirect3DTexture9 *texture = nullptr;
        D3DLOCKED_RECT locked;
        bool loaded = decoder->GetFrame(0, &frame) >= 0
            && frame->GetSize(&width, &height) >= 0
            && width > 0 && height > 0;
        if(loaded && gray && device->CreateTexture(width, height, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &texture, nullptr) >= 0) {
            loaded = wic->CreateFormatConverter(&converter) >= 0
                && converter->Initialize(frame, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom) >= 0;
        }
        else if(loaded) {
            texture = nullptr;
            loaded = wic->CreateFormatConverter(&converter) >= 0
                // Straight BGRA, which is D3DFMT_A8R8G8B8 in memory
                && converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom) >= 0
                && device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr) >= 0;
        }
        if(loaded) {
            loaded = texture->LockRect(0, &locked, nullptr, 0) >= 0;
            if(loaded) {
                loaded = converter->CopyPixels(nullptr, locked.Pitch, locked.Pitch * height, static_cast<BYTE *>(locked.pBits)) >= 0;
                texture->UnlockRect(0);
            }
        }
        if(!loaded) {
            release(texture);
        }
        release(converter);
        release(frame);
        return texture;
    }

    IDirect3DTexture9 *load_image(IDirect3DDevice9 *device, const wchar_t *path, UINT &width, UINT &height) noexcept {
        auto *wic = factory();
        IWICBitmapDecoder *decoder = nullptr;
        if(!wic || wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder) < 0) {
            return nullptr;
        }
        auto *texture = load_decoded(device, wic, decoder, false, width, height);
        release(decoder);
        return texture;
    }

    IDirect3DTexture9 *load_image(IDirect3DDevice9 *device, const void *data, std::size_t size, bool gray, UINT &width, UINT &height) noexcept {
        auto *wic = factory();
        IWICStream *stream = nullptr;
        IWICBitmapDecoder *decoder = nullptr;
        IDirect3DTexture9 *texture = nullptr;
        // Only read, whatever the signature says
        if(wic && wic->CreateStream(&stream) >= 0
        && stream->InitializeFromMemory(static_cast<BYTE *>(const_cast<void *>(data)), static_cast<DWORD>(size)) >= 0
        && wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder) >= 0) {
            texture = load_decoded(device, wic, decoder, gray, width, height);
        }
        release(decoder);
        release(stream);
        return texture;
    }

    struct ImageVertex {
        float x;
        float y;
        float z;
        float rhw;
        D3DCOLOR color;
        float u;
        float v;
    };
    #define IMAGE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

    void set_image_states(IDirect3DDevice9 *device) noexcept {
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(IMAGE_FVF);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    void draw_image(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const ImageCorner (&corners)[4], float alpha) noexcept {
        auto color = (static_cast<D3DCOLOR>(alpha * 255.0F + 0.5F) << 24) | 0xFFFFFF;
        ImageVertex vertices[4];
        for(int i = 0; i < 4; i++) {
            vertices[i] = { corners[i].x, corners[i].y, 0.0F, 1.0F, color, corners[i].u, corners[i].v };
        }
        device->SetTexture(0, texture);
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(ImageVertex));
    }
}
