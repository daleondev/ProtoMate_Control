#pragma once

#include "common/app/Application.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "runtime/ThreadGroup.hpp"
#include "ui/logging/Subscriber.hpp"

#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace ui
{
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
        void installLogSink();

      private:
        friend class runtime::ThreadGroup;
        // Priorities are local to this application's scheduler.
        static constexpr runtime::thread::Definition icc_THREAD{ .priority = 4, .stack_size = 32UZ * 1024UZ };
        static constexpr runtime::thread::Definition APPLICATION_THREAD{ .priority = 14,
                                                                         .stack_size = 16UZ * 1024UZ };
        static constexpr runtime::thread::Definition LOG_THREAD{ .priority = 20, .stack_size = 8UZ * 1024UZ };
#if defined(HAL_PLATFORM_LINUX)
        static constexpr runtime::thread::Definition CLI_THREAD{ .priority = 5, .stack_size = 64UZ * 1024UZ };
#else
        static constexpr runtime::thread::Definition CLI_THREAD{ .priority = 5, .stack_size = 16UZ * 1024UZ };
#endif

        [[= icc_THREAD]] void iccThread();
        [[= APPLICATION_THREAD]] void applicationThread();
        [[= CLI_THREAD]] void cliThread();
        [[= LOG_THREAD]] void logThread();

        pnm::msg::Bus m_bus;
        pnm::coro::Context m_iccContext;
        icc::Bridge m_bridge;
        logging::Subscriber m_logs;
        std::mutex m_mutex;
        std::condition_variable m_stopRequested;
        bool m_started{};
        bool m_stopping{};
        std::stop_source m_stopSource;
        runtime::ThreadGroup m_threads;
    };

    static_assert(app::Application<Application>);
}
