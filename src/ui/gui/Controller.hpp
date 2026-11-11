#pragma once
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/devices/itf/ITouch.hpp"
#include "hmi/Backend.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "scene3d/RenderService.hpp"
#include "util/WheelQueue.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
namespace ui::gui
{
    enum class State : std::uint8_t
    {
        Starting,
        Running,
        Unavailable,
        Fault,
        Stopped
    };
    struct Status
    {
        State state{ State::Starting };
        std::uint32_t frames{}, presented{}, missed_deadlines{}, maximum_render_us{}, underruns{}, errors{},
          queue_overflow{};
        std::uint32_t interactions{};
        std::uint32_t dma_errors{}, render_failures{}, input_overflow{};
        unsigned value{ hmi::INITIAL_GRAPHICS_VALUE };
        bool graphics_verified{}, animating{ true }, touch_available{}, touch_down{}, touch_cancelled{};
        std::uint16_t touch_x{}, touch_y{};
        std::chrono::steady_clock::time_point touch_time{};
        unsigned active_buffer{};
        unsigned brightness{};
        std::uint32_t draw_us{}, synchronize_us{};
        std::uint64_t total_draw_us{}, total_synchronize_us{}, image_transfer_us{};
        unsigned image_transfers{};
    };
    class Controller final
    {
      public:
        static constexpr unsigned DEFAULT_BENCHMARK_TRIANGLES{ 2000 };
        Controller(Controller&&) = delete;
        auto operator=(Controller&&) -> Controller& = delete;

        Controller();
        void backend(hmi::Backend& value) { m_backend = &value; }
        bool robotRate(unsigned fps, bool interactive = false);
        ~Controller();
        Controller(const Controller&) = delete;
        Controller& operator=(const Controller&) = delete;
        // GUI, touch and rendering workers use the application's shared stop token.
        // true means the host window requested application shutdown.
        bool run(const std::stop_token& stop);
        void acquireTouch(const std::stop_token& stop);
        void render3d(const std::stop_token& stop) { m_scene.run(stop); }
        bool scene(unsigned triangles = DEFAULT_BENCHMARK_TRIANGLES);
        scene3d::Status sceneStatus() const { return m_scene.status(); }
        bool bars();
        bool showcase();
        bool backlight(unsigned percent);
        Status status() const;

      private:
        static constexpr unsigned REQUEST_CAPACITY{ 8 };
        struct Engine;
        struct DeleteEngine
        {
            void operator()(Engine* /*engine*/) const;
        };
        hmi::Backend* m_backend{};
        enum class Command : std::uint8_t
        {
            Bars,
            Showcase,
            Backlight,
            Scene,
            Rate
        };
        struct Request
        {
            Command command;
            unsigned value;
            bool interactive{};
        };
        bool enqueue(Request request);
        bool waitPresentation(unsigned slot, const std::stop_token& stop);
        void commands(Engine& engine);
        void renderFrame(hal::display::State /*state*/, bool& illuminated);
        struct TouchSelection
        {
            std::optional<unsigned> primary;
            bool multiple{};
        };
        struct TouchSample
        {
            hal::device::TouchPoint point;
            unsigned contacts{};
        };
        static TouchSample selectTouch(const hal::util::Result<hal::device::TouchReport>& /*report*/,
                                       TouchSelection& /*selection*/);
        void publishTouch(const hal::util::Result<hal::device::TouchReport>& /*report*/,
                          hal::device::TouchPoint /*point*/,
                          bool multiple);
        mutable std::mutex m_mutex;
        Status m_status;
        hal::device::TouchReport m_input;
        util::WheelQueue m_wheel;
        std::array<Request, REQUEST_CAPACITY> m_requests{};
        unsigned m_head{}, m_count{};
        runtime::Notification m_notification, m_touchNotification;
        std::shared_ptr<hal::device::IDisplay> m_display;
        std::unique_ptr<Engine, DeleteEngine> m_engine;
        bool m_renderFailed{};
        scene3d::RenderService m_scene;
    };
}
