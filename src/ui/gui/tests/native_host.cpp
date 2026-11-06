#include "hal/linux/DisplayHost.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
namespace
{
    std::atomic_int phase{};
    std::atomic_int scenario{ -1 };
    std::atomic_bool correct{ true };
    std::array<double, 13> latencies{};
    unsigned latencyCount{};
    std::uint64_t inputAt{};
    unsigned sampleX{}, sampleY{};
    std::uint16_t beforePixel{};
    void mouse(Uint32 type, int x, int y)
    {
        SDL_Event e{};
        e.type = type;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
        SDL_PushEvent(&e);
    }
    void capture(const char* name, unsigned active)
    {
        auto p = hal::linux::display::framebuffer(active);
        unsigned foreground{};
        for (auto pixel : p)
            foreground += pixel != p[0];
        if (foreground < 10000)
            correct = false;
        auto* surface = SDL_CreateRGBSurfaceFrom(p.data(), 1024, 600, 16, 2048, 0xf800, 0x7e0, 0x1f, 0);
        if (!surface || SDL_SaveBMP(surface, name))
            correct = false;
        SDL_FreeSurface(surface);
    }
    void observe(void*, const hal::display::State& s, std::uint32_t window)
    {
        const auto n = s.vsyncs;
        if (inputAt) {
            const auto pixels = hal::linux::display::framebuffer(s.active);
            if (pixels[sampleY * 1024 + sampleX] != beforePixel) {
                latencies[latencyCount++] =
                  1000. * (SDL_GetPerformanceCounter() - inputAt) / SDL_GetPerformanceFrequency();
                inputAt = 0;
            }
        }

        if (std::getenv("GUI_TEST_STALL")) {
            if (n == 25)
                SDL_Delay(350);
            return;
        }
        struct Step
        {
            int x, y;
            const char* file;
        };
        static constexpr Step steps[]{ { 50, 118, "gui-home.bmp" },     { 50, 202, "gui-axes.bmp" },
                                       { 380, 118, "gui-robot.bmp" },   { 550, 118, "gui-cartesian.bmp" },
                                       { 720, 118, "gui-preview.bmp" }, { 890, 118, "gui-motors.bmp" },
                                       { 50, 286, "gui-programs.bmp" }, { 50, 370, "gui-io.bmp" },
                                       { 50, 454, "gui-settings.bmp" }, { 180, 554, "gui-jobs.bmp" },
                                       { 300, 554, "gui-alarms.bmp" },  { 50, 202, "gui-axis-input.bmp" },
                                       { 200, 434, "gui-keypad.bmp" } };
        if (n >= 30 && n < 30 + std::size(steps) * 45) {
            const auto i = (n - 30) / 45, t = (n - 30) % 45;
            if (t == 0) {
                sampleX = steps[i].x + 10;
                sampleY = steps[i].y + 20;
                beforePixel = hal::linux::display::framebuffer(s.active)[sampleY * 1024 + sampleX];
                inputAt = SDL_GetPerformanceCounter();
                mouse(SDL_MOUSEBUTTONDOWN, steps[i].x, steps[i].y);
            }
            if (t == 4)
                mouse(SDL_MOUSEBUTTONUP, steps[i].x, steps[i].y);
            if (t == 35) {
                capture(steps[i].file, s.active);
                phase = int(i + 1);
            }
        }
        if (n == 630) {
            SDL_SetWindowSize(SDL_GetWindowFromID(window), 1600, 600);
        }
        if (n == 650)
            mouse(SDL_MOUSEBUTTONDOWN, 338, 118); // letterboxed Home
        if (n == 654)
            mouse(SDL_MOUSEBUTTONUP, 338, 118);
        if (n == 675) {
            capture("gui-letterbox.bmp", s.active);
            phase = 14;
        }
        if (n == 680)
            mouse(SDL_MOUSEBUTTONDOWN, 488, 118); // viewport, then focus loss
        if (n == 684) {
            SDL_Event e{};
            e.type = SDL_WINDOWEVENT;
            e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&e);
        }
        if (n == 700) {
            mouse(SDL_MOUSEBUTTONDOWN, 1038, 365);
        } // cancel keypad after letterboxing
        if (n == 704) {
            mouse(SDL_MOUSEBUTTONUP, 1038, 365);
        }
        if (n == 710)
            mouse(SDL_MOUSEBUTTONDOWN, 338, 118);
        if (n == 714)
            mouse(SDL_MOUSEBUTTONUP, 338, 118);
        if (n >= 730 && n < 1000) {
            constexpr const char* names[]{ "ready", "disabled", "busy", "unconfigured", "stale", "fault" };
            const auto i = (n - 730) / 45, t = (n - 730) % 45;
            if (t == 0)
                scenario = int(i);
            if (t == 35) {
                char path[80];
                std::snprintf(path, sizeof(path), "gui-state-%s.bmp", names[i]);
                capture(path, s.active);
                phase = 15 + int(i);
            }
        }
        if (n == 1020) {
            std::sort(latencies.begin(), latencies.begin() + latencyCount);
            const double p95 = latencyCount ? latencies[(latencyCount * 95 - 1) / 100] : 0;
            std::fprintf(stderr, "HMI visible press feedback: samples=%u p95=%.2f ms\n", latencyCount, p95);
            if (latencyCount < 10 || p95 > 50)
                correct = false;
            if (auto* file = std::fopen("gui-latency.json", "w")) {
                std::fprintf(file, "{\"samples\":%u,\"p95_ms\":%.3f}\n", latencyCount, p95);
                std::fclose(file);
            }
            phase = 21;
            SDL_Event e{};
            e.type = SDL_QUIT;
            SDL_PushEvent(&e);
        }
    }
}
extern "C" bool gui_test_pixels_correct() { return correct; }
extern "C" int gui_test_scenario() { return scenario; }
extern "C" int gui_test_phase() { return phase; }
extern "C" int runtime_host_main(void (*kernel)())
{
    const auto result = hal::linux::display::run(kernel, observe);
    if (!hal::linux::display::cleanupComplete())
        return result;
    for (;;) {
        const timespec delay{ 0, 1000000 };
        nanosleep(&delay, nullptr);
    }
}
