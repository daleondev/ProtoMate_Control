#include "hal/linux/DisplayHost.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>
namespace
{
    std::atomic_int phase{};
    std::atomic_bool pixelsCorrect{ true };
    std::array<double, 40> latencies{};
    unsigned latencyCount{};
    Uint64 pendingInput{};
    std::atomic<double> latency95{};
    void mouse(Uint32 type, int x, int y)
    {
        SDL_Event event{};
        event.type = type;
        if (type == SDL_MOUSEMOTION) {
            event.motion.x = x;
            event.motion.y = y;
            event.motion.state = SDL_BUTTON_LMASK;
        }
        else {
            event.button.button = SDL_BUTTON_LEFT;
            event.button.x = x;
            event.button.y = y;
        }
        SDL_PushEvent(&event);
    }
    void capture(const char* name, unsigned active)
    {
        auto pixels = hal::linux::display::framebuffer(active);
        const bool details = std::string_view{ name }.find("details") != std::string_view::npos;
        const bool bars = std::string_view{ name }.find("bars") != std::string_view::npos;
        if (bars) {
            constexpr std::uint16_t expected[]{ 0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0 };
            for (unsigned x = 0; x < 1024; ++x) {
                if (pixels[300 * 1024 + x] != expected[x / 128]) {
                    pixelsCorrect = false;
                }
            }
        }
        else if (!details) {
            if (pixels[10 * 1024 + 10] != 0x10c4 || pixels[200 * 1024 + 400] != 0x1946) {
                pixelsCorrect = false;
            }
            unsigned glyphPixels{};
            for (unsigned y = 208; y < 244; ++y) {
                for (unsigned x = 504; x < 900; ++x) {
                    glyphPixels += pixels[y * 1024 + x] != 0x1946;
                }
            }
            if (glyphPixels < 100) {
                pixelsCorrect = false;
            }
        }
        SDL_Surface* surface =
          SDL_CreateRGBSurfaceFrom(pixels.data(), 1024, 600, 16, 2048, 0xf800, 0x7e0, 0x1f, 0);
        if (!surface || SDL_SaveBMP(surface, name) != 0) {
            std::fprintf(stderr, "capture failed: %s\n", SDL_GetError());
        }
        SDL_FreeSurface(surface);
    }
    void observe(void*, const hal::display::State& state, std::uint32_t windowId)
    {
        if (!state.vsyncs) {
            return;
        }
        const auto n = state.vsyncs;
        if (std::getenv("GUI_TEST_STALL")) {
            if (n == 25) {
                SDL_Delay(350);
            }
            return;
        }
        if (n == 20) {
            capture("gui-showcase.bmp", state.active);
            mouse(SDL_MOUSEBUTTONDOWN, 200, 230);
        }
        if (n == 22) {
            mouse(SDL_MOUSEBUTTONUP, 200, 230);
        }
        if (n == 40) {
            phase.store(1);
        }
        if (n == 50) {
            mouse(SDL_MOUSEBUTTONDOWN, 520, 280);
        }
        if (n == 53) {
            mouse(SDL_MOUSEMOTION, 850, 280);
        }
        if (n == 56) {
            mouse(SDL_MOUSEBUTTONUP, 850, 280);
        }
        if (n == 70) {
            phase.store(2);
            capture("gui-interaction.bmp", state.active);
        }
        if (n == 80) {
            SDL_SetWindowSize(SDL_GetWindowFromID(windowId), 1600, 600);
        }
        // 288 px horizontal letterbox. Correctly transformed touch toggles again.
        if (n == 90) {
            mouse(SDL_MOUSEBUTTONDOWN, 488, 230);
        }
        if (n == 92) {
            mouse(SDL_MOUSEBUTTONUP, 488, 230);
        }
        if (n == 110) {
            phase.store(3);
        }
        if (n == 120) {
            mouse(SDL_MOUSEBUTTONDOWN, 488, 230);
        }
        if (n == 122) {
            SDL_Event event{};
            event.type = SDL_WINDOWEVENT;
            event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&event);
        }
        if (n == 140) {
            phase.store(4);
        }
        if (n == 160) {
            capture("gui-bars.bmp", state.active);
            phase.store(5);
        }
        if (n == 190) {
            phase.store(6);
        }
        if (n == 200) {
            mouse(SDL_MOUSEBUTTONDOWN, 1008, 540);
        }
        if (n == 202) {
            mouse(SDL_MOUSEBUTTONUP, 1008, 540);
        }
        if (n == 215) {
            capture("gui-details.bmp", state.active);
            phase.store(7);
        }
        if (n == 230) {
            mouse(SDL_MOUSEBUTTONDOWN, 400, 540);
        }
        if (n == 232) {
            mouse(SDL_MOUSEBUTTONUP, 400, 540);
        }
        if (n == 245) {
            phase.store(8);
        }
        if (n >= 260 && n < 660) {
            if ((n - 260) % 10 == 0) {
                mouse(SDL_MOUSEBUTTONDOWN, 488, 230);
                pendingInput = SDL_GetPerformanceCounter();
            }
            if ((n - 260) % 10 == 5) {
                mouse(SDL_MOUSEBUTTONUP, 488, 230);
            }
            const auto pixels = hal::linux::display::framebuffer(state.active);
            if (pendingInput && pixels[212 * 1024 + 85] == 0x2cf1 && latencyCount < latencies.size()) {
                latencies[latencyCount++] =
                  1000. * (SDL_GetPerformanceCounter() - pendingInput) / SDL_GetPerformanceFrequency();
                pendingInput = 0;
            }
        }
        if (n == 670) {
            std::sort(latencies.begin(), latencies.begin() + latencyCount);
            latency95.store(latencyCount == 40 ? latencies[37] : 1000.);
            std::printf(
              "Input to framebuffer presentation: n=%u p95=%.3f ms\n", latencyCount, latency95.load());
        }
        if (n == 680) {
            SDL_Event event{};
            event.type = SDL_QUIT;
            SDL_PushEvent(&event);
        }
    }
}
extern "C" bool gui_test_pixels_correct()
{
    return pixelsCorrect.load() && latency95.load() > 0 && latency95.load() < 50;
}
extern "C" int gui_test_phase() { return phase.load(); }
extern "C" int runtime_host_main(void (*kernel)())
{
    const auto status = hal::linux::display::run(kernel, observe);
    if (!hal::linux::display::cleanupComplete()) {
        return status;
    }
    for (;;) {
        const timespec delay{ 0, 1000000 };
        nanosleep(&delay, nullptr);
    }
}
