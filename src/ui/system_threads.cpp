#include "Application.hpp"
#if defined(APPLICATION_ENABLE_P2P)
#include "common/Topics.hpp"
#endif

#include <chrono>

namespace ui
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
        {
            bool on{};
            std::unique_lock lock(m_mutex);
            while (!m_stopping) {
                on = !on;
                ++m_ledSequence;
                if (m_statusLed) {
                    if (on) { m_statusLed->turnOn(); }
                    else { m_statusLed->turnOff(); }
                }
#if defined(APPLICATION_ENABLE_P2P)
                lock.unlock();
                if (m_bridge) {
                    m_bus.topic<^^application::Topics::io_led_state>().publish(
                      application::LedState{.sequence = m_ledSequence, .on = on});
                }
                lock.lock();
#endif
                m_stopRequested.wait_for(lock, std::chrono::milliseconds{500}, [this] { return m_stopping; });
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
