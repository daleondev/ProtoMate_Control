#pragma once

#include "common/Application.hpp"
#include "runtime/ThreadGroup.hpp"

#include "icc/bridge/Bridge.hpp"

#include <condition_variable>
#include <mutex>

namespace ui
{
    class Application final
    {
      public:
        // Construction starts no threads. The endpoint must outlive the application.
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

      private:
        friend class runtime::ThreadGroup;
        // Priorities are local to this application's scheduler.
        static constexpr runtime::thread::Definition icc_THREAD{ .priority = 4, .stack_size = 32UZ * 1024UZ };
        static constexpr runtime::thread::Definition APPLICATION_THREAD{ .priority = 14,
                                                                         .stack_size = 16UZ * 1024UZ };

        [[= icc_THREAD]] void iccThread();
        [[= APPLICATION_THREAD]] void applicationThread();

        pnm::msg::Bus m_bus;
        pnm::coro::Context m_iccContext;
        icc::Bridge m_bridge;
        std::mutex m_mutex;
        std::condition_variable m_stopRequested;
        bool m_started{};
        bool m_stopping{};
        runtime::ThreadGroup m_threads;
    };

    static_assert(application::Application<Application>);
}
