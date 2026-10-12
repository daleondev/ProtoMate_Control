#include "Server.hpp"
#include "common/icc/catalog/services.hpp"
#include "runtime/synchronization/Notification.hpp"

namespace io::diagnostics
{
    Server::Server(pnm::msg::Bus& bus)
      : m_memory{ bus.service<^^icc::Services::io_memory>().serve(
          ::diagnostics::collectMemory, { .max_pending = 2 }) }
    {
    }

    void Server::run(std::stop_token stop)
    {
        auto notification{ std::make_shared<runtime::Notification>() };
        const auto ready{ pnm::msg::watch(m_memory, [notification] { notification->signal(); }) };
        const std::stop_callback cancellation{ stop, [notification] { notification->signal(); } };
        while (!stop.stop_requested()) {
            if (m_memory.poll() == 0) {
                notification->waitUntil(std::chrono::steady_clock::time_point::max());
            }
        }
        m_memory.close();
    }
}
