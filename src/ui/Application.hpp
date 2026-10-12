#pragma once

#include "common/Application.hpp"
#include "hal/devices/itf/ILed.hpp"
#include "runtime/ThreadGroup.hpp"

#if defined(APPLICATION_ENABLE_P2P)
#include "p2p/bridge/Bridge.hpp"
#include <optional>
#endif

#include <condition_variable>
#include <memory>
#include <mutex>

namespace ui
{
    class Application final
    {
      public:
        // Construction starts no threads. A missing LED is useful for host tests.
        explicit Application(std::shared_ptr<hal::device::ILed> statusLed = {});
#if defined(APPLICATION_ENABLE_P2P)
        // The endpoint must outlive the application.
        explicit Application(p2p::ITransport& transport, std::shared_ptr<hal::device::ILed> statusLed = {});
#endif
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
#if defined(APPLICATION_ENABLE_P2P)
        [[nodiscard]] p2p::ConnectionStatus connectionStatus() const;
#endif

      private:
        friend class runtime::ThreadGroup;
        // Priorities are local to this application's scheduler.
        static constexpr runtime::thread::Definition P2P_THREAD{ .priority = 4, .stack_size = 32UZ * 1024UZ };
        static constexpr runtime::thread::Definition APPLICATION_THREAD{ .priority = 14, .stack_size = 16UZ * 1024UZ };

#if defined(APPLICATION_ENABLE_P2P)
        [[= P2P_THREAD]] void p2pThread();
#endif
        [[= APPLICATION_THREAD]] void applicationThread();

#if defined(APPLICATION_ENABLE_P2P)
        pnm::msg::Bus m_bus;
        pnm::coro::Context m_p2pContext;
        std::optional<p2p::Bridge> m_bridge;
#endif
        std::shared_ptr<hal::device::ILed> m_statusLed;
        std::mutex m_mutex;
        std::condition_variable m_stopRequested;
        bool m_started{};
        bool m_stopping{};
        std::uint32_t m_ledSequence{};
        runtime::ThreadGroup m_threads;
    };

    static_assert(application::Application<Application>);
}
