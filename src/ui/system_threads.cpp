#include "Application.hpp"

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
            std::unique_lock lock(m_mutex);
            while (!m_stopping) {
                if (m_statusLed) {
                    m_statusLed->toggle();
                    m_stopRequested.wait_for(lock, std::chrono::milliseconds{500}, [this] { return m_stopping; });
                } else {
                    m_stopRequested.wait(lock, [this] { return m_stopping; });
                }
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
