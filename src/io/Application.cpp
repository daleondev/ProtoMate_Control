#include "Application.hpp"
#include "icc/catalog/actions.hpp"
#include "icc/catalog/services.hpp"
#include "icc/catalog/topics.hpp"

#include <stdexcept>

namespace io
{
    Application::Application(icc::ITransport& transport)
      : m_bridge(m_bus, m_iccContext, transport)
    {
        m_bridge.registerTopics<application::Topics>(application::TopicPublisher::IO);
        m_bridge.registerServices<application::Services>(application::ServiceProvider::IO);
        m_bridge.registerActions<application::Actions>(application::ActionProvider::IO);
        m_bridge.setConnectionCallback([this](icc::ConnectionStatus status) {
            if (status.state == icc::ConnectionState::Failed ||
                status.reason == icc::ConnectionReason::PeerStopped) {
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
            throw std::logic_error("IO application can only be started once, before stopping");
        }
        m_started = true;
        try {
            // The lifecycle entry waits on m_mutex until bridge startup finishes.
            m_threads.start(*this, "io");
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

    icc::ConnectionStatus Application::connectionStatus() const { return m_bridge.connectionStatus(); }
}
