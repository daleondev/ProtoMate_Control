#include "hal/linux/DisplayHost.hpp"
#include "runtime/thread.hpp"
#include "ui/gui/Controller.hpp"
#include <algorithm>
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
        const bool messages = std::getenv("GUI_TEST_MESSAGES");
        const int n = messages ? 0 : std::max(0, gui_test_scenario());

        s = {};
        s.sequence = unsigned(n + 2);
        if (messages) {
            const auto index = std::clamp(gui_test_scenario(), 0, 2);
            constexpr const char* texts[]{ "Connected to IO",
                                           "Motion rejected: Axis 1 is busy",
                                           "Driver fault: reference must be renewed" };
            s.outcome.operation = index + 1;
            s.outcome.accepted = index == 0;
            s.outcome.severity = static_cast<ui::gui::hmi::Severity>(index);
            std::snprintf(s.outcome.message.data(), s.outcome.message.size(), "%s", texts[index]);
        }
        s.simulation = true;
        s.connected = n >= 0;
        s.fresh = n >= 0 && n < 4;
        s.configured = n >= 0 && n != 3;
        s.robot_configured = s.configured;
        s.initialized = n >= 0;
        s.enabled = n >= 0 && n != 1;
        s.failed = n == 5;
        s.pose_valid = n >= 0 && n != 3;
        s.feedback_valid = s.pose_valid;
        s.commanded = s.feedback = { .3, .5, .03 };
        s.tcp = s.feedback_tcp = { 310, 120, 59 };
        s.combined_state = n == 5 ? 4 : 1;
        for (unsigned i = 0; i < 3; ++i) {
            auto& a = s.axes[i];
            a.commanded = a.feedback = i == 2 ? 30 : (i == 0 ? .3 : .5) * 180 / 3.141592653589793;
            a.minimum = i == 2 ? 0 : -170;
            a.maximum = i == 2 ? 120 : 170;
            a.max_speed = 30;
            std::snprintf(a.driver.data(), a.driver.size(), "%s", i == 0 ? "DM542T" : "TMC2209");
            a.referenced = s.pose_valid;
            a.valid = s.pose_valid;
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
    ui::gui::hmi::Kind diagnosticKind{ ui::gui::hmi::Kind::MemoryUI };
    ui::gui::hmi::Outcome submit(const ui::gui::hmi::Command& c) override
    {
        diagnosticKind = c.kind;
        return {};
    }
    void diagnostics(unsigned offset, ui::gui::hmi::DiagnosticPage& p) override
    {
        using enum ui::gui::hmi::Kind;
        p = {};
        p.available = true;
        p.shared = true;
        p.allocator = 1;
        p.age_ms = 1200;
        p.total = 8;
        p.io = diagnosticKind == MemoryIO || diagnosticKind == MemoryIOStack;
        p.stacks = diagnosticKind == MemoryIOStack || diagnosticKind == MemoryUIStack;
        constexpr const char* heaps[]{ "Capacity [B]",   "Reserved [B]",   "Used [B]",
                                       "Available [B]",  "Releasable [B]", "Free blocks",
                                       "Mapped regions", "Mapped [B]",     "Usage [%]" };
        constexpr const char* threads[]{ "ui.gui",    "ui.render3d", "ui.touch",  "ui.icc",
                                         "ui.motion", "ui.cli",      "io.motion", "io.icc" };
        p.total = p.stacks ? 8 : 9;
        constexpr std::uint64_t heapValues[]{ 0, 131072, 87381, 43691, 12288, 12, 2, 32768, 66 };
        p.capacity = 131072;
        p.used = 87381;
        for (unsigned i = offset; i < p.total && p.count < 6; ++i) {
            auto& r = p.rows[p.count++];
            std::snprintf(r.name.data(), r.name.size(), "%s", p.stacks ? threads[i] : heaps[i]);
            r.values = p.stacks ? std::array<std::uint64_t, 4>{ 8 + i, 16384, 2048 + i * 100, 4096 + i * 500 }
                                : std::array<std::uint64_t, 4>{ heapValues[i], 0, 0, 0 };
        }
    }
    unsigned jobs(unsigned offset, std::span<ui::gui::hmi::Job> out, bool preview) override
    {
        constexpr const char* states[]{ "Running", "Completed", "Rejected" };
        constexpr const char* reasons[]{ "Axis 1 move", "Robot joint destination", "Busy: robot job #42" };
        unsigned count{};
        for (unsigned i = offset; i < 3 && count < out.size(); ++i) {
            auto& j = out[count++];
            j = {};
            j.id = 42 - i;
            j.terminal = i != 0;
            j.preview = preview;
            j.seconds = 1.25;
            j.endpoint = { 200, -100, 320 };
            std::snprintf(j.status.data(), j.status.size(), "%s", states[i]);
            std::snprintf(j.detail.data(), j.detail.size(), "%s", reasons[i]);
        }
        return count;
    }
    ui::gui::hmi::Outcome hold(const ui::gui::hmi::Command&, ui::gui::hmi::layout::Bounds) override
    {
        return {};
    }
    void input(const ui::gui::hmi::Input&) override {}
    void release() override {}
};
extern "C" bool gui_test_pixels_correct();
extern "C" void runtime_host_completed(int status)
{
    hal::linux::display::completed(status);
    while (!hal::linux::display::cleanup_complete()) {
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
    const auto deadline = std::chrono::steady_clock::now() + (std::getenv("GUI_TEST_PAGES") ? 60s : 35s);
    const bool detailTest = std::getenv("GUI_TEST_DETAIL");
    bool interactive{}, refined{};
    unsigned idleCount{};
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        if (detailTest) {
            const auto state = gui.sceneStatus();
            if (gui_test_phase() == 1 && state.acquired_triangles && state.acquired_triangles < 9000) {
                interactive = true;
            }
            if (gui_test_phase() >= 2 && state.acquired_triangles == 36376) {
                refined = true;
            }
            if (gui_test_phase() == 3) {
                if (!idleCount) {
                    idleCount = state.rendered;
                }
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
    if (std::getenv("GUI_TEST_MESSAGES")) {
        return renderingPassed && gui_test_pixels_correct() && gui_test_phase() == 25 &&
                   s.state == ui::gui::State::Stopped && !s.errors
                 ? 0
                 : 1;
    }
    if (std::getenv("GUI_TEST_KEYBOARD")) {
        return renderingPassed && gui_test_pixels_correct() && gui_test_phase() == 24 &&
                   s.state == ui::gui::State::Stopped && !s.errors
                 ? 0
                 : 1;
    }
    if (std::getenv("GUI_TEST_WHEEL")) {
        return renderingPassed && gui_test_pixels_correct() && gui_test_phase() == 23 &&
                   s.state == ui::gui::State::Stopped && !s.errors
                 ? 0
                 : 1;
    }
    if (std::getenv("GUI_TEST_PAGES")) {
        return renderingPassed && gui_test_pixels_correct() && gui_test_phase() == 22 &&
                   s.state == ui::gui::State::Stopped && !s.errors
                 ? 0
                 : 1;
    }
    passed &= gui_test_pixels_correct() && renderingPassed && gui_test_phase() == 21 &&
              s.state == ui::gui::State::Stopped && s.presented > 40 && s.queue_overflow == 1;
    std::printf("checks: pixels=%d run=%d phase=%d state=%u overflow=%u\n",
                gui_test_pixels_correct(),
                renderingPassed,
                gui_test_phase(),
                unsigned(s.state),
                s.queue_overflow);
    std::printf("GUI test %s: frames=%u presentations=%u max_render_us=%u errors=%u\n",
                passed ? "PASS" : "FAIL",
                s.frames,
                s.presented,
                s.maximum_render_us,
                s.errors);
    return passed ? 0 : 1;
}
