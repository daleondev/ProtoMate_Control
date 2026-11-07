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
extern "C" int gui_test_scenario();
// Presentation/error fixtures only; real motion is exercised by simulation.hmi.
class ScreenFixture final : public ui::gui::hmi::Backend
{
  public:
    void read(ui::gui::hmi::Snapshot& s) override
    {
        const int n = gui_test_scenario();
        s = {};
        s.sequence = unsigned(n + 2);
        s.simulation = true;
        s.connected = n >= 0;
        s.fresh = n >= 0 && n < 4;
        s.configured = n >= 0 && n != 3;
        s.robotConfigured = s.configured;
        s.initialized = n >= 0;
        s.enabled = n >= 0 && n != 1;
        s.failed = n == 5;
        s.poseValid = n >= 0 && n != 3;
        s.feedbackValid = s.poseValid;
        s.commanded = s.feedback = { .3, .5, .03 };
        s.tcp = s.feedbackTcp = { 310, 120, 59 };
        s.combinedState = n == 5 ? 4 : 1;
        for (unsigned i = 0; i < 3; ++i) {
            auto& a = s.axes[i];
            a.commanded = a.feedback = i == 2 ? 30 : 30 + i * 10;
            a.referenced = s.poseValid;
            a.valid = s.poseValid;
            a.encoder = i == 0;
            a.owner = n == 2 ? 42 : 0;
        }
        constexpr const char* names[]{ "Ready",
                                       "Disabled: initialize then enable",
                                       "Busy: robot job #42 owns all axes",
                                       "Physical configuration missing",
                                       "Telemetry stale: last pose frozen",
                                       "ICC heartbeat timeout: restart required; stop unconfirmed" };
        std::snprintf(
          s.condition.data(), s.condition.size(), "%s", n < 0 ? "Awaiting IO telemetry" : names[n]);
    }
    ui::gui::hmi::Outcome submit(const ui::gui::hmi::Command&) override { return {}; }
    unsigned jobs(unsigned, std::span<ui::gui::hmi::Job>, bool) override { return 0; }
    ui::gui::hmi::Outcome hold(const ui::gui::hmi::Command&, int, int, int, int) override { return {}; }
    void input(const ui::gui::hmi::Input&) override {}
    void release() override {}
};
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
    ScreenFixture backend;
    ui::gui::Controller gui;
    gui.backend(backend);
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
    auto scene = runtime::thread::create({ .name = "scene.test", .priority = 16, .stack_size = 16384 },
                                         [&] { gui.render3d(stop.get_token()); });
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    const bool detailTest = std::getenv("GUI_TEST_DETAIL");
    bool interactive{}, refined{};
    unsigned idleCount{};
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        if (detailTest) {
            const auto state = gui.sceneStatus();
            if (gui_test_phase() == 1 && state.acquiredTriangles && state.acquiredTriangles < 9000)
                interactive = true;
            if (gui_test_phase() >= 2 && state.acquiredTriangles == 36376)
                refined = true;
            if (gui_test_phase() == 3) {
                if (!idleCount)
                    idleCount = state.rendered;
                passed &= state.rendered == idleCount;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    if (!stop.stop_requested()) {
        passed = false;
        stop.request_stop();
    }
    rendering.join();
    touch.join();
    scene.join();
    const auto s = gui.status();
    if (detailTest) {
        passed &= renderingPassed && interactive && refined && idleCount &&
                  s.state == ui::gui::State::Stopped && !s.errors;
        std::printf("LOD input test: interactive=%u refined=%u idle=%u %s\n",
                    interactive,
                    refined,
                    idleCount,
                    passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    passed &= gui_test_pixels_correct() && renderingPassed && gui_test_phase() == 21 &&
              s.state == ui::gui::State::Stopped && s.presented > 40 && s.queueOverflow == 1;
    std::printf("GUI test %s: frames=%u presentations=%u max_render_us=%u errors=%u\n",
                passed ? "PASS" : "FAIL",
                s.frames,
                s.presented,
                s.maximumRenderUs,
                s.errors);
    return passed ? 0 : 1;
}
