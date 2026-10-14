#pragma once

#include "common/app/Application.hpp"
#include "common/app/State.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "io/diagnostics/Server.hpp"
#include "io/motion/Controller.hpp"
#include "io/motion/Server.hpp"
#include "runtime/ThreadGroup.hpp"

#include <mutex>
#include <stop_token>

namespace io
{
    namespace logging
    {
        class Sink;
    }

    class Application final
    {
      public:
        // Construction initializes logging; application workers start in start().
        // The endpoint must outlive the application.
        explicit Application(icc::ITransport& transport);
        ~Application();
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        // start() is one-shot and returns after scheduling startup, without
        // waiting for the peer. Serialize start(), join() and destruction.
        void start();
        // Thread-safe, idempotent notification, also safe from bridge callbacks.
        void requestStop();
        // Waits for shutdown; does not initiate it. Never call from our workers.
        void join();
        [[nodiscard]] icc::ConnectionStatus connectionStatus() const;
        [[nodiscard]] app::State state() const noexcept { return m_state.get(); }
        // Use in the M4 entrypoint, whose process contains only IO. Linux keeps
        // global routing on UI and passes the IO sink to its producers explicitly.
        void installLogSink();

      private:
        friend class runtime::ThreadGroup;
        static constexpr runtime::thread::Definition ICC_THREAD{ .priority = 2, .stack_size = 32UZ * 1024UZ };
        static constexpr runtime::thread::Definition APPLICATION_THREAD{ .priority = 14,
                                                                         .stack_size = 16UZ * 1024UZ };
        static constexpr runtime::thread::Definition DIAGNOSTIC_THREAD{ .priority = 20,
                                                                        .stack_size = 16UZ * 1024UZ };

        [[= ICC_THREAD]] void iccThread();
        [[= APPLICATION_THREAD]] void applicationThread();
        [[= DIAGNOSTIC_THREAD]] void diagnosticThread();
        void publishState();

        pnm::msg::Bus m_bus;
        pnm::coro::Context m_iccContext;
        icc::Bridge m_bridge;
        app::ControllerState m_state;
        pnm::msg::Topic<app::State> m_stateTopic;
        pnm::coro::Context::Timer m_stateTimer;
        std::shared_ptr<logging::Sink> m_logSink;
        diagnostics::Server m_diagnostics;
        std::mutex m_mutex;
        bool m_started{};
        bool m_stopping{};
        std::stop_source m_stopSource;
        // Created on start; shared enable remains off until explicitly requested.
        std::unique_ptr<motion::Controller> m_motion;
        std::unique_ptr<motion::Server> m_motorServer;
        // Destroy/join workers before any of the state their entries access.
        runtime::ThreadGroup m_threads;
    };

    static_assert(app::Application<Application>);
}
