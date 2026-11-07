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
                if (latencyCount < latencies.size())
                    latencies[latencyCount++] =
                      1000. * (SDL_GetPerformanceCounter() - inputAt) / SDL_GetPerformanceFrequency();
                else
                    correct = false;
                inputAt = 0;
            }
        }

        if (std::getenv("GUI_TEST_STALL")) {
            if (n == 25)
                SDL_Delay(350);
            return;
        }
        if (std::getenv("GUI_TEST_DETAIL")) {
            if (n == 30) {
                mouse(SDL_MOUSEBUTTONDOWN, 400, 300);
                phase = 1;
            }
            if (n > 30 && n <= 90) {
                SDL_Event e{};
                e.type = SDL_MOUSEMOTION;
                e.motion.state = SDL_BUTTON_LMASK;
                e.motion.x = 400 + int(n - 30);
                e.motion.y = 300;
                SDL_PushEvent(&e);
            }
            if (n == 91) {
                mouse(SDL_MOUSEBUTTONUP, 460, 300);
                phase = 2;
            }
            if (n == 180)
                phase = 3;
            if (n == 240) {
                SDL_Event e{};
                e.type = SDL_QUIT;
                SDL_PushEvent(&e);
            }
            return;
        }
        if (std::getenv("GUI_TEST_WHEEL")) {
            static std::uint64_t baseline{}, closer{};
            static std::array<std::uint16_t, 640 * 300> closerPixels{};
            const auto hash = [&] {
                std::uint64_t h = 14695981039346656037ull;
                auto p = hal::linux::display::framebuffer(s.active);
                for (unsigned y = 150; y < 450; ++y)
                    for (unsigned x = 104; x < 744; ++x) {
                        h ^= p[y * 1024 + x];
                        h *= 1099511628211ull;
                    }
                return h;
            };
            const auto wheel = [&](float delta, int x, int y, bool flipped = false) {
                SDL_Event e{};
                e.type = SDL_MOUSEWHEEL;
                // SDL2-compat rewrites pushed precision from integer y. Fractional
                // deltas are verified at the bounded handoff/recognizer boundary.
                e.wheel.preciseY = delta;
                e.wheel.y = int(delta);
                e.wheel.direction = flipped ? SDL_MOUSEWHEEL_FLIPPED : SDL_MOUSEWHEEL_NORMAL;
#if SDL_VERSION_ATLEAST(2, 26, 0)
                e.wheel.mouseX = x;
                e.wheel.mouseY = y;
#else
                SDL_WarpMouseInWindow(SDL_GetWindowFromID(window), x, y);
#endif
                SDL_PushEvent(&e);
            };
            if (n == 120)
                baseline = hash();
            if (n == 130)
                wheel(1.f, 400, 300);
            if (n == 240) {
                closer = hash();
                const auto pixels = hal::linux::display::framebuffer(s.active);
                for (unsigned y = 0; y < 300; ++y)
                    std::copy_n(pixels.begin() + (y + 150) * 1024 + 104, 640, closerPixels.begin() + y * 640);
                correct = correct && closer != baseline;
            }
            if (n == 250)
                wheel(-1.f, 400, 300);
            if (n == 360)
                correct = correct && hash() == baseline;
            if (n == 370)
                wheel(1, 800, 300); // status card must not zoom
            if (n == 480)
                correct = correct && hash() == baseline;
            if (n == 490)
                wheel(1.f, 400, 300, true);
            if (n == 600)
                correct = correct && hash() != baseline && hash() != closer;
            if (n == 610)
                wheel(-1.f, 400, 300, true);
            if (n == 720)
                correct = correct && hash() == baseline;
            if (n == 730) {
                SDL_Event e{};
                e.type = SDL_WINDOWEVENT;
                e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                SDL_PushEvent(&e);
                // Leaving an already unfocused window must not restore focus.
                e.window.event = SDL_WINDOWEVENT_LEAVE;
                SDL_PushEvent(&e);
            }
            if (n == 740)
                wheel(1, 400, 300);
            if (n == 840)
                correct = correct && hash() == baseline;
            if (n == 850) {
                SDL_Event e{};
                e.type = SDL_WINDOWEVENT;
                e.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
                SDL_PushEvent(&e);
                SDL_SetWindowSize(SDL_GetWindowFromID(window), 1600, 600);
            }
            if (n == 860)
                wheel(1, 50, 250); // letterbox is not scene space
            if (n == 960)
                correct = correct && hash() == baseline;
            if (n == 970)
                wheel(1.f, 688, 300);
            if (n == 1080) {
                unsigned changed{};
                const auto pixels = hal::linux::display::framebuffer(s.active);
                for (unsigned y = 0; y < 300; ++y)
                    for (unsigned x = 0; x < 640; ++x)
                        changed += pixels[(y + 150) * 1024 + x + 104] != closerPixels[y * 640 + x];
                // Repeated inverse float scales can differ by a few edge pixels.
                // An additional scroll would change thousands, not <=32 /192000.
                correct = correct && changed <= 32;
                std::fprintf(stderr, "Letterboxed wheel round-trip: %u changed edge pixels\n", changed);
                phase = 23;
                SDL_Event e{};
                e.type = SDL_QUIT;
                SDL_PushEvent(&e);
                std::fprintf(
                  stderr, "Wheel, focus, letterboxing, exactly-once: %s\n", correct ? "PASS" : "FAIL");
            }
            return;
        }
        if (std::getenv("GUI_TEST_PAGES")) {
            struct PageStep
            {
                int x, y;
                const char* name;
            };
            static constexpr PageStep pages[]{ { 50, 58, "home" },
                                               { 50, 154, "axis-jog" },
                                               { 720, 182, "axis-move" },
                                               { 910, 182, "axis-settings" },
                                               { 274, 184, "axis-2" },
                                               { 378, 184, "axis-3" },
                                               { 544, 184, "axis-3-jog" },
                                               { 730, 248, "axis-timed" },
                                               { 910, 248, "axis-hold" },
                                               { 540, 248, "axis-step" },
                                               { 890, 118, "more" },
                                               { 740, 250, "robot" },
                                               { 840, 184, "robot-advanced" },
                                               { 840, 184, "robot-joints" },
                                               { 544, 184, "robot-point" },
                                               { 660, 118, "cartesian" },
                                               { 890, 118, "more" },
                                               { 740, 328, "preview" },
                                               { 840, 184, "preview-line" },
                                               { 890, 118, "more" },
                                               { 740, 404, "motor-move" },
                                               { 710, 184, "motor-home" },
                                               { 840, 184, "motor-defaults" },
                                               { 50, 346, "io-digital" },
                                               { 780, 118, "io-drivers" },
                                               { 50, 538, "settings-general" },
                                               { 544, 118, "settings-robot" },
                                               { 890, 118, "settings-heap" },
                                               { 920, 184, "settings-heap" },
                                               { 920, 490, "settings-heap-next" },
                                               { 270, 184, "settings-io-heap" },
                                               { 590, 184, "settings-io-stacks" },
                                               { 920, 490, "settings-io-stacks-next" },
                                               { 770, 184, "settings-graphics" },
                                               { 920, 490, "settings-graphics-next" },
                                               { 50, 58, "home-return" } };
            if (n >= 30 && n < 30 + std::size(pages) * 35) {
                const auto i = (n - 30) / 35, t = (n - 30) % 35;
                if (t == 0)
                    mouse(SDL_MOUSEBUTTONDOWN, pages[i].x, pages[i].y);
                if (t == 4)
                    mouse(SDL_MOUSEBUTTONUP, pages[i].x, pages[i].y);
                if (t == 28) {
                    char path[96];
                    std::snprintf(path, sizeof(path), "page-%s.bmp", pages[i].name);
                    capture(path, s.active);
                }
            }
            if (n == 30 + std::size(pages) * 35) {
                phase = 22;
                SDL_Event e{};
                e.type = SDL_QUIT;
                SDL_PushEvent(&e);
            }
            return;
        }
        struct Step
        {
            int x, y;
            const char* file;
        };
        static constexpr Step steps[]{ { 50, 58, "gui-home.bmp" },      { 50, 154, "gui-axes.bmp" },
                                       { 890, 118, "gui-robot.bmp" },   { 660, 118, "gui-cartesian.bmp" },
                                       { 890, 118, "gui-preview.bmp" }, { 890, 118, "gui-motors.bmp" },
                                       { 50, 250, "gui-programs.bmp" }, { 50, 346, "gui-io.bmp" },
                                       { 50, 538, "gui-settings.bmp" }, { 180, 554, "gui-jobs.bmp" },
                                       { 50, 442, "gui-alarms.bmp" },   { 50, 154, "gui-axis-input.bmp" },
                                       { 720, 182, "gui-keypad.bmp" } };
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
            if (t == 12 && (i == 2 || i == 4 || i == 5 || i == 12))
                mouse(SDL_MOUSEBUTTONDOWN,
                      i == 12 ? 650 : 740,
                      i == 2   ? 250
                      : i == 4 ? 328
                      : i == 5 ? 404
                               : 320);
            if (t == 16 && (i == 2 || i == 4 || i == 5 || i == 12))
                mouse(SDL_MOUSEBUTTONUP,
                      i == 12 ? 650 : 740,
                      i == 2   ? 250
                      : i == 4 ? 328
                      : i == 5 ? 404
                               : 320);
            if (t == 35) {
                capture(steps[i].file, s.active);
                phase = int(i + 1);
            }
        }
        if (n == 630) {
            SDL_SetWindowSize(SDL_GetWindowFromID(window), 1600, 600);
        }
        if (n == 650)
            mouse(SDL_MOUSEBUTTONDOWN, 338, 58); // letterboxed Home
        if (n == 654)
            mouse(SDL_MOUSEBUTTONUP, 338, 58);
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
            SDL_Event focus{};
            focus.type = SDL_WINDOWEVENT;
            focus.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
            SDL_PushEvent(&focus);
            mouse(SDL_MOUSEBUTTONDOWN, 1038, 365);
        } // cancel keypad after letterboxing
        if (n == 704) {
            mouse(SDL_MOUSEBUTTONUP, 1038, 365);
        }
        if (n == 710)
            mouse(SDL_MOUSEBUTTONDOWN, 338, 58);
        if (n == 714)
            mouse(SDL_MOUSEBUTTONUP, 338, 58);
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
            const auto count = std::min<std::size_t>(latencyCount, latencies.size());
            // Only thirteen samples: bounded insertion sort avoids the large
            // introsort machinery for this native test-side percentile.
            for (std::size_t i = 1; i < count; ++i) {
                const auto value = latencies[i];
                auto j = i;
                while (j && latencies[j - 1] > value) {
                    latencies[j] = latencies[j - 1];
                    --j;
                }
                latencies[j] = value;
            }
            const double p95 = count ? latencies[(count * 95 - 1) / 100] : 0;
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
