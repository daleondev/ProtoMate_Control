#include "Subscriber.hpp"
#include "common/icc/catalog/topics.hpp"
#include "runtime/synchronization/Notification.hpp"

namespace ui::logging
{
    Subscriber::Subscriber(pnm::msg::Bus& bus, Output output)
      : m_subscription{ bus.topic<^^icc::Topics::io_logs>().subscribe(
          [output = std::move(output)](const std::string& record) {
              // This is already formatted by IO. Do not re-log through global
              // Pneumo routing or replace its timestamp with UI's receive time.
              auto line{ std::string{ "[IO] " } + record.substr(0, icc::MAX_LOG_BYTES) };
              if (line.back() != '\n') { line += '\n'; }
              output(line);
          }, icc::LOG_SUBSCRIPTION) }
    {
    }

    void Subscriber::run(std::stop_token stop)
    {
        // A readiness callback already in flight may finish after unregistering.
        // Its shared notification remains alive until that callback returns.
        auto notification{ std::make_shared<runtime::Notification>() };
        const auto ready{ pnm::msg::watch(m_subscription, [notification] { notification->signal(); }) };
        const std::stop_callback cancellation{ stop, [notification] { notification->signal(); } };
        while (!stop.stop_requested()) {
            if (m_subscription.pollSome(1) == 0) {
                notification->waitUntil(std::chrono::steady_clock::time_point::max());
            }
        }
    }

    pnm::msg::SubscriptionStatistics Subscriber::statistics() const
    {
        return m_subscription.statistics();
    }
}
