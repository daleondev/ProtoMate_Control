#include "Application.hpp"

namespace ui
{
    void Application::iccThread() { m_iccContext.run(); }

    void Application::applicationThread()
    {
        {
            std::unique_lock lock(m_mutex);
            m_stopRequested.wait(lock, [this] { return m_stopping; });
        }
        // Keep the context alive until bridge shutdown has drained its work.
        m_bridge.stop();
        m_iccContext.stop();
    }
}
