#include "Application.hpp"
#if defined(APPLICATION_ENABLE_P2P)
#include "common/Topics.hpp"
#endif

#include <chrono>

namespace io
{
#if defined(APPLICATION_ENABLE_P2P)
    void Application::p2pThread()
    {
        if (m_bridge) {
            m_p2pContext.run();
        }
    }
#endif

    void Application::applicationThread()
    {
#if defined(APPLICATION_ENABLE_P2P)
        auto subscription = m_bus.topic<^^application::Topics::io_led_state>().subscribe(
          [this](const application::LedState& state) {
              m_ledSequence = state.sequence;
              if (m_statusLed) {
                  if (state.on) { m_statusLed->turnOn(); }
                  else { m_statusLed->turnOff(); }
              }
          }, pnm::msg::SubscriptionOptions::latestOnly());
        auto readiness = pnm::msg::watch(subscription, [this] {
            {
                const std::scoped_lock lock(m_mutex);
                m_topicReady = true;
            }
            m_stopRequested.notify_all();
        });
#endif
        {
            std::unique_lock lock(m_mutex);
            while (!m_stopping) {
                m_stopRequested.wait(lock, [this] { return m_stopping || m_topicReady; });
                if (m_stopping) { break; }
                m_topicReady = false;
#if defined(APPLICATION_ENABLE_P2P)
                lock.unlock();
                subscription.poll();
                lock.lock();
#endif
            }
            if (m_statusLed) {
                m_statusLed->turnOff();
            }
        }
#if defined(APPLICATION_ENABLE_P2P)
        // Keep the context alive until bridge shutdown has drained its work.
        if (m_bridge) {
            m_bridge->stop();
            m_p2pContext.stop();
        }
#endif
    }
}
