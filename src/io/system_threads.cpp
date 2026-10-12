#include "Application.hpp"

namespace io
{
    void Application::p2pThread() { m_p2pContext.run(); }

    void Application::applicationThread()
    {
        {
            std::unique_lock lock(m_mutex);
            m_stopRequested.wait(lock, [this] { return m_stopping; });
        }
        // Future IO workers must stop admission and drain motion here, while
        // communication is still available. Bridge::stop() needs its runner alive.
        m_bridge.stop();
        m_p2pContext.stop();
    }
}
