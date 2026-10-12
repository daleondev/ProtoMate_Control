#include "Application.hpp"
#include "p2p/topics.hpp"
#include "p2p/services.hpp"
#include "p2p/actions.hpp"

#include <stdexcept>

namespace ui
{
    Application::Application(p2p::ITransport& transport)
      : m_bridge(m_bus, m_p2pContext, transport)
    {
        m_bridge.registerTopics<application::Topics>(application::TopicPublisher::UI);
        m_bridge.registerServices<application::Services>(application::ServiceProvider::UI);
        m_bridge.registerActions<application::Actions>(application::ActionProvider::UI);
        m_bridge.setConnectionCallback([this](p2p::ConnectionStatus status) {
            if (status.state == p2p::ConnectionState::Failed || status.reason == p2p::ConnectionReason::PeerStopped) {
                requestStop();
            }
        });
    }

    Application::~Application()
    {
        requestStop();
        join();
    }

    void Application::start()
    {
        std::unique_lock lock(m_mutex);
        if (m_started || m_stopping) {
            throw std::logic_error("UI application can only be started once, before stopping");
        }
        m_started = true;
        try {
            m_threads.start(*this, "ui");
            m_bridge.start();
        } catch (...) {
            lock.unlock();
            requestStop();
            join();
            throw;
        }
    }

    void Application::requestStop()
    {
        {
            std::scoped_lock lock(m_mutex);
            m_stopping = true;
        }
        m_stopRequested.notify_all();
    }

    void Application::join() { m_threads.join(); }

    p2p::ConnectionStatus Application::connectionStatus() const
    {
        return m_bridge.connectionStatus();
    }
}
