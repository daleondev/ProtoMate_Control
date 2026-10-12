#include "Application.hpp"
#if defined(APPLICATION_ENABLE_P2P)
#include "common/Topics.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace ui
{
    Application::Application(std::shared_ptr<hal::device::ILed> statusLed)
      : m_statusLed(std::move(statusLed))
    {
    }

#if defined(APPLICATION_ENABLE_P2P)
    Application::Application(p2p::ITransport& transport, std::shared_ptr<hal::device::ILed> statusLed)
      : m_bridge(std::in_place, m_bus, m_p2pContext, transport)
      , m_statusLed(std::move(statusLed))
    {
        m_bridge->registerTopics<application::Topics>(application::TopicPublisher::UI);
        m_bridge->setConnectionCallback([this](p2p::ConnectionStatus status) {
            if (status.state == p2p::ConnectionState::Failed || status.reason == p2p::ConnectionReason::PeerStopped) {
                requestStop();
            }
        });
    }

#endif

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
#if defined(APPLICATION_ENABLE_P2P)
            if (m_bridge) {
                m_bridge->start();
            }
#endif
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

#if defined(APPLICATION_ENABLE_P2P)
    p2p::ConnectionStatus Application::connectionStatus() const
    {
        return m_bridge ? m_bridge->connectionStatus() : p2p::ConnectionStatus{};
    }
#endif
}
