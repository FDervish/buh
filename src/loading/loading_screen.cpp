// SPDX-License-Identifier: GPL-3.0-only
//
// Maps load on another thread while Halo's own loading screen keeps being drawn, with an animated background (the
// shader in loading_screen_shader.hlsl over the picture in loading_screen_background.png) instead of Halo's.

#include "loading_screen.hpp"
#include "loading_screen_shader.h"
#include "../console.hpp"
#include "../halo.hpp"
#include "../map/map_load.hpp"
#include "../memory.hpp"
#include "../render.hpp"
#include "../sprites/image.hpp"

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

// The most often a frame is drawn while a map loads
#define FRAME_SECONDS (1.0 / 60.0)

// How long the light takes to sweep across; it waits half that again before the next
#define SWEEP_SECONDS 4.0

// A gap this long between frames of the background means it's another loading screen, which starts the sweep over
#define NEW_LOADING_SCREEN_SECONDS 0.5

// buh_loading_screen_test's default: one sweep across and the wait after it
#define TEST_SECONDS 6.0F

// The most buh_loading_screen_test and buh_loading_screen_minimum take
#define MAXIMUM_SECONDS 60.0F

// Halo's state for its singleplayer loading screen, and its "not yet" time
#define SINGLEPLAYER_LOADING_SCREEN 1
#define NOT_YET 0xFFFFFFFF

#define INIT_TXT_MAXIMUM_SIZE 0x10000

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Buh {
    static bool enabled = false;

    // buh_loading_screen_minimum: how long a singleplayer map's loading screen stays up at least, before the game starts
    static float minimum_seconds = 0.0F;
    static std::uint32_t minimum_milliseconds = 0;

    static IDirect3DTexture9 *background = nullptr;
    static IDirect3DPixelShader9 *shader = nullptr;
    static IDirect3DDevice9 *background_device = nullptr;
    static UINT background_width = 0;
    static UINT background_height = 0;
    static bool background_unavailable = false;
    static IDirect3DStateBlock9 *state_block = nullptr;

    static LARGE_INTEGER sweep_start = {};
    static LARGE_INTEGER last_background = {};
    static float last_alpha = 1.0F;
    static bool background_drawn = false;

    // Halo's loading screen, and its clock (milliseconds)
    using HaloLoadingScreen = void (*)();
    using HaloClock = std::uint32_t (*)();
    static HaloLoadingScreen halo_loading_screen = nullptr;
    static HaloClock halo_clock = nullptr;

    // What Halo's loading screen goes by: its state (0 when it isn't up), when it went up and when it comes down (by
    // Halo's clock, fading out over the last 750 ms), and what else Halo clears when it comes down
    struct HaloLoadingScreenGlobals {
        std::uint32_t *state;
        std::uint32_t *shown_at;
        std::uint32_t *ends_at;
        std::uint32_t *handle;
        std::uint32_t *substate;
        std::uint16_t *strings[2];
    };
    static HaloLoadingScreenGlobals halo_screen = {};

    extern "C" {
        const void *buh_halo_loading_background = nullptr;
        void buh_loading_background_asm() noexcept;
    }

    struct LoadJob {
        const char *map;
        MapLoadFunction load;
        std::uint32_t result;
    };

    static bool multithreaded(IDirect3DDevice9 *device) noexcept {
        D3DDEVICE_CREATION_PARAMETERS parameters;
        return device->GetCreationParameters(&parameters) >= 0 && (parameters.BehaviorFlags & D3DCREATE_MULTITHREADED);
    }

    static HMODULE this_module() noexcept {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&this_module), &module);
        return module;
    }

    static void release_background() noexcept {
        if(background) {
            background->Release();
            background = nullptr;
        }
        if(shader) {
            shader->Release();
            shader = nullptr;
        }
        background_device = nullptr;
    }

    // The picture (from buh's resources) and the shader, once per device
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(background_device == device) {
            return true;
        }
        if(background_unavailable) {
            return false;
        }
        release_background();
        D3DCAPS9 caps;
        auto *module = this_module();
        // RT_RCDATA, as a narrow string
        auto *resource = module ? FindResourceA(module, "LOADING_SCREEN_BACKGROUND", MAKEINTRESOURCEA(10)) : nullptr;
        auto *data = resource ? LockResource(LoadResource(module, resource)) : nullptr;
        if(!data || device->GetDeviceCaps(&caps) < 0 || caps.PixelShaderVersion < D3DPS_VERSION(3, 0)
        || device->CreatePixelShader(loading_screen_shader, &shader) < 0
        || !(background = load_image(device, data, SizeofResource(module, resource), true, background_width, background_height))) {
            if(shader) {
                shader->Release();
                shader = nullptr;
            }
            background_unavailable = true;
            console_error("buh: the loading screen's background can't be drawn here (it needs pixel shader 3.0), so Halo's stays");
            return false;
        }
        background_device = device;
        return true;
    }

    // The animated background over the viewport, faded in by alpha
    static bool draw_background(IDirect3DDevice9 *device, float alpha) noexcept {
        if(!prepare(device)) {
            return false;
        }
        if(!state_block && device->CreateStateBlock(D3DSBT_ALL, &state_block) < 0) {
            state_block = nullptr;
            return false;
        }
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if(static_cast<double>(now.QuadPart - last_background.QuadPart) / frequency.QuadPart > NEW_LOADING_SCREEN_SECONDS) {
            sweep_start = now;
        }
        last_background = now;

        // The picture covers the viewport, cropped to keep its shape
        D3DVIEWPORT9 viewport;
        device->GetViewport(&viewport);
        float width = static_cast<float>(viewport.Width);
        float height = static_cast<float>(viewport.Height);
        float shape = (width / height) / (static_cast<float>(background_width) / background_height);
        const float constants[5][4] = {
            { width, height, 0.0F, 0.0F },
            { static_cast<float>(SWEEP_SECONDS), 0.0F, 0.0F, 0.0F },
            { static_cast<float>(static_cast<double>(now.QuadPart - sweep_start.QuadPart) / frequency.QuadPart), 0.0F, 0.0F, 0.0F },
            { alpha, 0.0F, 0.0F, 0.0F },
            { static_cast<float>(viewport.X), static_cast<float>(viewport.Y), std::min(shape, 1.0F), std::min(1.0F / shape, 1.0F) }
        };

        // Halo keeps track of the states it set, so they all go back
        state_block->Capture();
        set_image_states(device);
        device->SetPixelShader(shader);
        device->SetPixelShaderConstantF(0, constants[0], 5);
        float left = viewport.X - 0.5F;
        float top = viewport.Y - 0.5F;
        const ImageCorner corners[4] = {
            { left, top, 0.0F, 0.0F },
            { left + width, top, 1.0F, 0.0F },
            { left + width, top + height, 1.0F, 1.0F },
            { left, top + height, 0.0F, 1.0F }
        };
        draw_image(device, background, corners, alpha);
        state_block->Apply();
        last_alpha = alpha;
        return true;
    }

    // Instead of Halo drawing its own background on its loading screen, with the same fade. Halo's draws if this can't.
    extern "C" bool buh_loading_background(std::uint32_t color) noexcept {
        auto *device = halo_device();
        if(!enabled || !device || !draw_background(device, static_cast<float>(color >> 24) / 255.0F)) {
            return false;
        }
        background_drawn = true;
        return true;
    }

    // Halo's loading screen up from the start, set the way Halo does for a singleplayer game. It comes down at ends_at,
    // or for a map, when Halo sets that after loading it.
    static void bring_up_loading_screen(std::uint32_t ends_at) noexcept {
        *halo_screen.ends_at = ends_at;
        *halo_screen.shown_at = NOT_YET;
        *halo_screen.handle = NOT_YET;
        *halo_screen.substate = 0;
        *halo_screen.strings[0] = 0;
        *halo_screen.strings[1] = 0;
        *halo_screen.state = SINGLEPLAYER_LOADING_SCREEN;
    }

    // A frame of Halo's loading screen, if it's up
    static void draw_loading_screen(IDirect3DDevice9 *device) noexcept {
        if(*halo_screen.state == 0 || device->TestCooperativeLevel() != D3D_OK) {
            return;
        }
        device->BeginScene();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0F, 0);
        background_drawn = false;
        halo_loading_screen();
        // While the map's tags are being replaced, Halo's loading screen can't find its own, so it draws nothing
        if(!background_drawn && *halo_screen.state != 0) {
            draw_background(device, last_alpha);
        }
        device->EndScene();

        // Through the swap chain, so to anything hooking the device's Present (buh's own callbacks too) it isn't one
        // of Halo's frames
        IDirect3DSwapChain9 *swap_chain = nullptr;
        if(device->GetSwapChain(0, &swap_chain) >= 0) {
            swap_chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
            swap_chain->Release();
        }
    }

    static DWORD WINAPI load_thread(void *parameter) {
        auto &job = *static_cast<LoadJob *>(parameter);
        job.result = job.load(job.map);
        return 0;
    }

    // As much stack as Halo's own main thread, which the load would otherwise run on
    static SIZE_T stack_size() noexcept {
        auto *base = reinterpret_cast<const std::byte *>(GetModuleHandle(nullptr));
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
        return std::max<SIZE_T>(nt->OptionalHeader.SizeOfStackReserve, 0x100000);
    }

    // How much longer the loading screen has to stay up for the minimum, in milliseconds by Halo's clock
    static std::int32_t minimum_left(std::uint32_t shown_at, std::uint32_t minimum) noexcept {
        return static_cast<std::int32_t>(minimum - (halo_clock() - shown_at));
    }

    // Draw Halo's loading screen and keep the window's messages moving while a map loads on another thread (if it
    // does), then for as long as it has to stay up for the minimum. Quitting has to wait for both.
    static void keep_loading_screen_up(IDirect3DDevice9 *device, HANDLE thread, std::uint32_t shown_at, std::uint32_t minimum) noexcept {
        // Frames are timed with a high resolution timer where Windows has one (10 1803 on), since waits otherwise go by
        // about 16 ms, which would hold the loading screen to 30 frames a second
        auto *timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        bool loaded = thread == nullptr;
        bool quit = false;
        WPARAM quit_code = 0;
        LARGE_INTEGER frequency, now, last_frame;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&last_frame);
        last_frame.QuadPart -= frequency.QuadPart;
        for(;;) {
            MSG message;
            while(PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                if(message.message == WM_QUIT) {
                    quit = true;
                    quit_code = message.wParam;
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            QueryPerformanceCounter(&now);
            if(static_cast<double>(now.QuadPart - last_frame.QuadPart) / frequency.QuadPart >= FRAME_SECONDS) {
                draw_loading_screen(device);
                last_frame = now;
            }
            auto left = minimum_left(shown_at, minimum);
            if(loaded && left <= 0) {
                break;
            }

            // Until the next frame, unless a message comes, the load finishes or the minimum is up first
            QueryPerformanceCounter(&now);
            double due = FRAME_SECONDS - static_cast<double>(now.QuadPart - last_frame.QuadPart) / frequency.QuadPart;
            if(loaded) {
                due = std::min(due, left / 1000.0);
            }
            HANDLE handles[2];
            DWORD count = 0;
            if(!loaded) {
                handles[count++] = thread;
            }
            DWORD timeout = due > 0.0 ? static_cast<DWORD>(due * 1000.0) + 1 : 0;
            if(timer && due > 0.0) {
                // In 100 ns, negative being from now
                LARGE_INTEGER when;
                when.QuadPart = -static_cast<LONGLONG>(due * 10000000.0) - 1;
                if(SetWaitableTimer(timer, &when, 0, nullptr, nullptr, FALSE)) {
                    handles[count++] = timer;
                    timeout = INFINITE;
                }
            }
            auto woke = MsgWaitForMultipleObjects(count, handles, FALSE, timeout, QS_ALLINPUT);
            if(woke == WAIT_FAILED) {
                break;
            }
            if(!loaded && woke == WAIT_OBJECT_0) {
                loaded = true;
            }
        }
        if(timer) {
            CloseHandle(timer);
        }
        if(quit) {
            PostQuitMessage(static_cast<int>(quit_code));
        }
    }

    static std::uint32_t run_map_load(const char *map, MapLoadFunction load) noexcept {
        if(!enabled || is_ui_map(map)) {
            return load(map);
        }

        // Without its loading screen up (chimera_block_loading_screen, say), Halo shows a singleplayer map before its
        // first tick has run the scripts that set it up, like fading to black for a cutscene. Up, Halo keeps it there
        // for a second after the map loads while the game runs underneath, then fades it into the game.
        if(singleplayer() && *halo_screen.state == 0) {
            bring_up_loading_screen(NOT_YET);
        }

        // A singleplayer map is held back until the loading screen has been up for the minimum, so the game doesn't
        // start (or play its cutscene) behind it. A server's game goes on without it.
        auto shown_at = *halo_screen.shown_at != NOT_YET ? *halo_screen.shown_at : halo_clock();
        auto minimum = singleplayer() && *halo_screen.state != 0 ? minimum_milliseconds : 0;

        auto *device = halo_device();
        LoadJob job = { map, load, 0 };
        HANDLE thread = nullptr;
        if(device && multithreaded(device)) {
            thread = CreateThread(nullptr, stack_size(), load_thread, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        }
        if(!thread) {
            job.result = load(map);
        }
        if(device && (thread || minimum_left(shown_at, minimum) > 0)) {
            keep_loading_screen_up(device, thread, shown_at, minimum);
        }
        if(thread) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        return job.result;
    }

    static void reset() noexcept {
        if(state_block) {
            state_block->Release();
            state_block = nullptr;
        }
    }

    static bool is_blank(char c) noexcept {
        return c == ' ' || c == '\t' || c == '"';
    }

    // The next word of a line, quotes left off
    static std::string_view next_word(std::string_view &line) noexcept {
        while(!line.empty() && is_blank(line.front())) {
            line.remove_prefix(1);
        }
        std::size_t length = 0;
        while(length < line.size() && !is_blank(line[length])) {
            length++;
        }
        auto word = line.substr(0, length);
        line.remove_prefix(length);
        return word;
    }

    // Whether init.txt turns the loading screen on: its last buh_loading_screen line that sets it
    static bool init_txt_turns_on() noexcept {
        auto *handle = CreateFileA("init.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        std::string text(INIT_TXT_MAXIMUM_SIZE, '\0');
        DWORD read = 0;
        ReadFile(handle, text.data(), INIT_TXT_MAXIMUM_SIZE, &read, nullptr);
        CloseHandle(handle);
        text.resize(read);

        static const char command[] = "buh_loading_screen";
        bool on = false;
        std::string_view rest = text;
        while(!rest.empty()) {
            auto end = rest.find_first_of("\r\n");
            auto line = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            auto name = next_word(line);
            if(name.size() != sizeof(command) - 1 || _strnicmp(name.data(), command, name.size()) != 0) {
                continue;
            }
            bool value;
            if(parse_bool(std::string(next_word(line)), value)) {
                on = value;
            }
        }
        return on;
    }

    // Up over whatever is on screen for that long, the last 750 ms fading out, the way Halo takes it down after a map
    // 2.5 rather than 2.500000
    static std::string seconds_text(float seconds) {
        auto text = std::to_string(seconds);
        text.erase(text.find_last_not_of('0') + 1);
        if(text.back() == '.') {
            text.pop_back();
        }
        return text;
    }

    static void test_command(const std::vector<std::string> &arguments) {
        float seconds = TEST_SECONDS;
        if(!arguments.empty() && (!parse_float(arguments[0], seconds) || seconds <= 0.0F || seconds > MAXIMUM_SECONDS)) {
            console_error("usage: buh_loading_screen_test [seconds], up to 60");
            return;
        }
        // In a server's game Halo's loading screen also goes with joining it, so it's left alone there
        if(!singleplayer()) {
            console_error("buh: buh_loading_screen_test only works in singleplayer and the menus");
            return;
        }
        if(*halo_screen.state != 0) {
            console_error("buh: a loading screen is already up");
            return;
        }
        bring_up_loading_screen(halo_clock() + static_cast<std::uint32_t>(seconds * 1000.0F));
        console_print(("buh_loading_screen_test: " + seconds_text(seconds) + " seconds" + (enabled ? "" : ", Halo's own since buh_loading_screen is false")).c_str());
    }

    static void minimum_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty()) {
            float seconds;
            if(!parse_float(arguments[0], seconds) || seconds < 0.0F || seconds > MAXIMUM_SECONDS) {
                console_error("usage: buh_loading_screen_minimum [seconds], 0 to 60");
                return;
            }
            minimum_seconds = seconds;
            minimum_milliseconds = static_cast<std::uint32_t>(seconds * 1000.0F + 0.5F);
        }
        console_print(("buh_loading_screen_minimum: " + seconds_text(minimum_seconds)).c_str());
    }

    static void loading_screen_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_loading_screen [true|false]");
            return;
        }
        console_print(enabled ? "buh_loading_screen: true" : "buh_loading_screen: false");
        auto *device = halo_device();
        if(enabled && device && !multithreaded(device)) {
            console_error("buh: maps only load behind the loading screen with buh_loading_screen true in init.txt when Halo starts");
        }
    }

    void set_up_loading_screen() noexcept {
        // Halo only makes its Direct3D device multithreaded if asked, and drawing while the map loads on another
        // thread needs that. The device is made before init.txt runs, so init.txt is read here to decide.
        // and edx, 4; add edi, edx -> or edi, 4
        auto *behavior_flags = find_signature({ 0x83, 0xE2, 0x04, 0x03, 0xFA, 0x8B, 0x54, 0x24, 0x28, 0x57, 0x52, 0x8B, 0x15 });

        // Halo's loading screen, as far as where it comes down: whether it's up (mov eax, [state]), when it went up
        // (the clock into [shown at]), when it comes down ([ends at]), then all it clears doing that
        auto *loading_screen = find_signature({ 0xA1, -1, -1, -1, -1, 0x81, 0xEC, 0x18, 0x04, 0x00, 0x00, 0x57, 0x33, 0xFF, 0x3B, 0xC7,
            0x0F, 0x84, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x56, 0x83, 0xCE, 0xFF, 0x3B, 0xC6, 0xC7, 0x44, 0x24, 0x08, 0x00,
            0x00, 0x80, 0x3F, 0x75, 0x0A, 0xE8, -1, -1, -1, -1, 0xA3, -1, -1, -1, -1, 0x39, 0x35, -1, -1, -1, -1, 0x0F, 0x84,
            -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x8B, 0x0D, -1, -1, -1, -1, 0x3B, 0xC1, 0x72, 0x35, 0x89, 0x35, -1, -1, -1,
            -1, 0x89, 0x35, -1, -1, -1, -1, 0x89, 0x35, -1, -1, -1, -1, 0x5E, 0x89, 0x3D, -1, -1, -1, -1, 0x89, 0x3D, -1, -1,
            -1, -1, 0x66, 0x89, 0x3D, -1, -1, -1, -1, 0x66, 0x89, 0x3D, -1, -1, -1, -1, 0x5F, 0x81, 0xC4, 0x18, 0x04, 0x00,
            0x00, 0xC3 });

        // Where it draws its background: the call before add esp, 0xC; push ebp; push 2; push -1
        auto *background_call = find_signature({ 0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x0C, 0x55, 0x6A, 0x02, 0x6A, 0xFF, 0x8D, 0x44, 0x24, 0x24 });
        if(!behavior_flags || !loading_screen || !background_call) {
            return;
        }
        halo_loading_screen = reinterpret_cast<HaloLoadingScreen>(loading_screen);
        halo_clock = reinterpret_cast<HaloClock>(branch_target(loading_screen + 0x2B));
        auto address = [loading_screen](std::size_t offset) {
            return *reinterpret_cast<std::uint32_t **>(loading_screen + offset);
        };
        halo_screen.state = address(0x01);
        halo_screen.shown_at = address(0x17);
        halo_screen.ends_at = address(0x37);
        halo_screen.handle = address(0x5E);
        halo_screen.substate = address(0x6B);
        halo_screen.strings[0] = reinterpret_cast<std::uint16_t *>(address(0x72));
        halo_screen.strings[1] = reinterpret_cast<std::uint16_t *>(address(0x79));
        buh_halo_loading_background = branch_target(background_call);
        retarget_call(background_call, reinterpret_cast<const void *>(buh_loading_background_asm));
        if(init_txt_turns_on()) {
            const std::uint8_t multithreaded_flag[5] = { 0x83, 0xCF, 0x04, 0x90, 0x90 };
            overwrite(behavior_flags, multithreaded_flag, sizeof(multithreaded_flag));
        }
        set_map_load_runner(run_map_load);
        add_reset_callback(reset);
        add_console_command("buh_loading_screen", "[true|false]", 0, 1, loading_screen_command);
        add_console_command("buh_loading_screen_test", "[seconds]", 0, 1, test_command);
        add_console_command("buh_loading_screen_minimum", "[seconds]", 0, 1, minimum_command);
    }
}
