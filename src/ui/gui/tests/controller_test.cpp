#include "hal/linux/DisplayHost.hpp"
#include "runtime/thread.hpp"
#include "ui/gui/Controller.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stop_token>
#include <thread>
#include <tx_api.h>
extern "C" int gui_test_phase();
extern "C" bool gui_test_pixels_correct();
extern "C" void runtime_host_completed(int status)
{
    hal::linux::display::completed(status);
    while (!hal::linux::display::cleanupComplete()) {
        tx_thread_sleep(1);
    }
}
int main()
{
    using namespace std::chrono_literals;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ui::gui::Controller gui;
    std::stop_source stop;
    bool passed = true;
    bool renderingPassed{}; // Read only after joining the rendering worker.
    for (unsigned i = 0; i < 8; ++i) {
        passed &= gui.backlight(10);
    }
    passed &= !gui.backlight(10); // bounded request capacity, accounted overflow
    auto rendering = runtime::thread::create({ .name = "gui.test", .priority = 8, .stack_size = 32768 }, [&] {
        renderingPassed = gui.run(stop.get_token());
        stop.request_stop();
    });
    auto touch = runtime::thread::create({ .name = "touch.test", .priority = 7, .stack_size = 8192 },
                                         [&] { gui.acquireTouch(stop.get_token()); });
    if (std::getenv("GUI_TEST_STALL")) {
        const auto limit = std::chrono::steady_clock::now() + 3s;
        while (gui.status().state != ui::gui::State::Fault && std::chrono::steady_clock::now() < limit) {
            std::this_thread::sleep_for(5ms);
        }
        stop.request_stop();
        rendering.join();
        touch.join();
        return gui.status().state == ui::gui::State::Fault ? 0 : 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    int previous{};
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const auto phase = gui_test_phase();
        if (phase != previous) {
            const auto s = gui.status();
            if (phase == 1) {
                passed &= !s.animating && s.interactions == 1;
            }
            if (phase == 2) {
                passed &= s.value >= 89 && s.value <= 91 && !s.touchDown;
            }
            if (phase == 3) {
                passed &= s.animating;
            }
            if (phase == 4) {
                passed &= !s.touchDown && s.animating;
                passed &= gui.bars();
            }
            if (phase == 5) {
                passed &= gui.showcase();
            }
            if (phase == 6) {
                passed &= s.state == ui::gui::State::Running && !s.errors && !s.underruns;
            }
            std::printf("phase=%d pass=%d interactions=%u value=%u animation=%d touch=%d\n",
                        phase,
                        passed,
                        s.interactions,
                        s.value,
                        s.animating,
                        s.touchDown);
            previous = phase;
        }
        std::this_thread::sleep_for(5ms);
    }
    if (!stop.stop_requested()) {
        passed = false;
        stop.request_stop();
    }
    rendering.join();
    touch.join();
    const auto s = gui.status();
    passed &= gui_test_pixels_correct() && renderingPassed && previous == 8 &&
              s.state == ui::gui::State::Stopped && s.presented > 40 && s.queueOverflow == 1;
    std::printf("GUI test %s: frames=%u presentations=%u max_render_us=%u errors=%u\n",
                passed ? "PASS" : "FAIL",
                s.frames,
                s.presented,
                s.maximumRenderUs,
                s.errors);
    return passed ? 0 : 1;
}
