#include "Application.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"
#include "io/logging/Sink.hpp"

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
      , m_logSink{ std::make_shared<logging::Sink>(m_bus) }
      , m_diagnostics{ m_bus }
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

        runtime::thread::publish_attributes({ .name = "io.logging", .priority = 20, .stack_size = 8192U });
        pnm::log::initialize();
        pnm::log::info(m_logSink, "IO-Controller started");
    }

    Application::~Application()
    {
        requestStop();
        join();
        static_cast<void>(m_logSink->close());
    }

    void Application::start()
    {
        std::unique_lock lock(m_mutex);
        if (m_started || m_stopping) {
            throw std::logic_error("IO application can only be started once, before stopping");
        }
        m_started = true;
        try {
            m_motion = std::make_unique<motion::Controller>(
              MOTOR_CONFIGURATION, motion::Controller::DriverFactory{}, m_logSink);
            m_motorServer = std::make_unique<motion::Server>(m_bus, *m_motion);
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
        m_stopSource.request_stop();
    }

    void Application::join() { m_threads.join(); }

    icc::ConnectionStatus Application::connectionStatus() const { return m_bridge.connectionStatus(); }

    void Application::installLogSink()
    {
        if (!pnm::log::set_default_sink(pnm::log::Level::Trace, pnm::log::Level::Critical, m_logSink)) {
            throw std::runtime_error("failed to configure IO logging");
        }
    }

    void Application::iccThread() { m_iccContext.run(); }

    void Application::diagnosticThread() { m_diagnostics.run(m_stopSource.get_token()); }

    void Application::applicationThread()
    {
        {
            std::scoped_lock lock(m_mutex); // Wait until startup has finished.
        }
        try {
            m_motorServer->run(m_stopSource.get_token());
        } catch (const std::exception& error) {
            pnm::log::error(m_logSink, "Motor service failed: {}", error.what());
            requestStop();
        }
        if (m_motion) {
            m_motion->disable();
        }
        // Queued logger entries can outlive shutdown, but must no longer publish.
        static_cast<void>(m_logSink->close());
        // Keep the context alive until bridge shutdown has drained its work.
        m_bridge.stop();
        m_iccContext.stop();
    }
}
