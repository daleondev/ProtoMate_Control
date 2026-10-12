#pragma once

#include "common/Application.hpp"
#include "p2p/bridge/Bridge.hpp"
#include "runtime/ThreadGroup.hpp"

#include <condition_variable>
#include <mutex>

namespace ui
{
    class Application final
    {
      public:
        // The endpoint must outlive the application. Construction starts no threads.
        explicit Application(p2p::ITransport& transport);
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
        [[nodiscard]] p2p::ConnectionStatus connectionStatus() const;

      private:
        friend class runtime::ThreadGroup;
        // UI work stays below IO communication on the shared H753 scheduler.
        static constexpr runtime::thread::Definition P2P_THREAD{ .priority = 4, .stack_size = 32UZ * 1024UZ };
        static constexpr runtime::thread::Definition APPLICATION_THREAD{ .priority = 14, .stack_size = 16UZ * 1024UZ };

        [[= P2P_THREAD]] void p2pThread();
        [[= APPLICATION_THREAD]] void applicationThread();

        pnm::msg::Bus m_bus;
        pnm::coro::Context m_p2pContext;
        p2p::Bridge m_bridge;
        std::mutex m_mutex;
        std::condition_variable m_stopRequested;
        bool m_started{};
        bool m_stopping{};
        runtime::ThreadGroup m_threads;
    };

    static_assert(application::Application<Application>);
}
