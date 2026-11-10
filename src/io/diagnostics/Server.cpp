#include "Server.hpp"
#include "common/icc/catalog/services.hpp"
#include "runtime/synchronization/Notification.hpp"

namespace io::diagnostics
{
    Server::Server(pnm::msg::Bus& bus)
      : m_profile{ bus.service<^^icc::Services::io_profile>().serve(::diagnostics::control_profile,
                                                                    { .max_pending = 2 }) }
      , m_memory{ bus.service<^^icc::Services::io_memory>().serve(::diagnostics::collect_memory,
                                                                  { .max_pending = 2 }) }
    {
    }

    void Server::run(const std::stop_token& stop)
    {
        constexpr auto profile_poll_interval{ std::chrono::milliseconds{ 100 } };
        auto notification{ std::make_shared<runtime::Notification>() };
        const auto profile_ready{ pnm::msg::watch(m_profile, [notification] { notification->signal(); }) };
        const auto ready{ pnm::msg::watch(m_memory, [notification] { notification->signal(); }) };
        const std::stop_callback cancellation{ stop, [notification] { notification->signal(); } };
        while (!stop.stop_requested()) {
            m_profile.poll();
#if !defined(HAL_PLATFORM_LINUX)
            ::diagnostics::poll_profiling();
#endif
            if (m_memory.poll() == 0) {
                notification->waitUntil(std::chrono::steady_clock::now() + profile_poll_interval);
            }
        }
        m_profile.close();
        m_memory.close();
    }
}
