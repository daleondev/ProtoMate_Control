#include "Application.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"

#include <stdexcept>

namespace io
{
    namespace
    {
        using namespace pnm::units::literals;
        // Motor-shaft units, retaining the previous application's placeholder
        // reference coordinates until the mechanics are calibrated.
        constexpr motion::Controller::Configuration MOTOR_CONFIGURATION{
            motion::Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16U },
            motion::Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16U },
            motion::Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16U }
        };
    }

    Application::Application(icc::ITransport& transport)
      : m_bridge(m_bus, m_iccContext, transport)
    {
        m_bridge.registerTopics<icc::Topics>(icc::TopicPublisher::IO);
        m_bridge.registerServices<icc::Services>(icc::ServiceProvider::IO);
        m_bridge.registerActions<icc::Actions>(icc::ActionProvider::IO);
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
            m_motion = std::make_unique<motion::Controller>(MOTOR_CONFIGURATION);
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

    void Application::iccThread() { m_iccContext.run(); }

    void Application::applicationThread()
    {
        {
            std::unique_lock lock(m_mutex);
            m_stopRequested.wait(lock, [this] { return m_stopping; });
        }
        if (m_motion) {
            m_motion->disable();
        }
        // Keep the context alive until bridge shutdown has drained its work.
        m_bridge.stop();
        m_iccContext.stop();
    }
}
