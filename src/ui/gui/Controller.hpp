#pragma once
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/devices/itf/ITouch.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "scene3d/RenderService.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
namespace ui::gui
{
    enum class State
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
        std::uint32_t frames{}, presented{}, missedDeadlines{}, maximumRenderUs{}, underruns{}, errors{},
          queueOverflow{};
        std::uint32_t interactions{};
        std::uint32_t dmaErrors{}, renderFailures{}, inputOverflow{};
        unsigned value{ 42 };
        bool graphicsVerified{}, animating{ true }, touchAvailable{}, touchDown{}, touchCancelled{};
        std::uint16_t touchX{}, touchY{};
        std::chrono::steady_clock::time_point touchTime{};
        unsigned activeBuffer{};
        std::uint32_t drawUs{}, synchronizeUs{};
        std::uint64_t totalDrawUs{}, totalSynchronizeUs{}, imageTransferUs{};
        unsigned imageTransfers{};
    };
    class Controller final
    {
      public:
        Controller();
        ~Controller();
        Controller(const Controller&) = delete;
        Controller& operator=(const Controller&) = delete;
        // GUI, touch and rendering workers use the application's shared stop token.
        // true means the host window requested application shutdown.
        bool run(std::stop_token stop);
        void acquireTouch(std::stop_token stop);
        void render3d(std::stop_token stop) { m_scene.run(stop); }
        bool scene(unsigned triangles = 2000);
        scene3d::Status sceneStatus() const { return m_scene.status(); }
        bool bars();
        bool showcase();
        bool backlight(unsigned percent);
        Status status() const;

      private:
        struct Engine;
        enum class Command
        {
            Bars,
            Showcase,
            Backlight,
            Scene
        };
        struct Request
        {
            Command command;
            unsigned value;
        };
        bool enqueue(Request request);
        bool waitPresentation(unsigned slot, std::stop_token stop);
        void commands(Engine& engine);
        mutable std::mutex m_mutex;
        Status m_status;
        std::array<Request, 8> m_requests{};
        unsigned m_head{}, m_count{};
        runtime::Notification m_notification, m_touchNotification;
        std::shared_ptr<hal::device::IDisplay> m_display;
        std::unique_ptr<Engine> m_engine;
        bool m_renderFailed{};
        scene3d::RenderService m_scene;
    };
}
