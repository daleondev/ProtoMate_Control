#include "Application.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"
#include "ui/cli/Console.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/filesystem.hpp"
#include "ui/cli/motor.hpp"
#include "ui/cli/utilities.hpp"
#include "ui/logging/Sink.hpp"

#include <cstdio>
#include <stdexcept>

#include "pneumo/logging.hpp"

namespace ui
{
    Application::Application(icc::ITransport& transport)
      : m_bridge(m_bus, m_iccContext, transport)
      , m_logs{ m_bus, cli::writeLog }
      , m_motors{ m_bus }
    {
        m_bridge.registerTopics<icc::Topics>(icc::TopicPublisher::UI);
        m_bridge.registerServices<icc::Services>(icc::ServiceProvider::UI);
        m_bridge.registerActions<icc::Actions>(icc::ActionProvider::UI);
        m_bridge.setConnectionCallback([this](icc::ConnectionStatus status) {
            if (status.state == icc::ConnectionState::Failed ||
                status.reason == icc::ConnectionReason::PeerStopped) {
                requestStop();
            }
        });

        runtime::thread::publish_attributes({ .name = "ui.logging", .priority = 20, .stack_size = 8192U });
        pnm::log::initialize();
        installLogSink();
        pnm::log::info("UI-Controller started");
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
        m_stopSource.request_stop();
    }

    void Application::join() { m_threads.join(); }

    icc::ConnectionStatus Application::connectionStatus() const { return m_bridge.connectionStatus(); }

    void Application::installLogSink()
    {
        if (!pnm::log::set_default_sink(
              pnm::log::Level::Trace, pnm::log::Level::Critical, std::make_shared<logging::Sink>())) {
            throw std::runtime_error("failed to configure UI logging");
        }
    }

    void Application::iccThread() { m_iccContext.run(); }

    void Application::logThread()
    {
        try {
            m_logs.run(m_stopSource.get_token());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Log subscriber failed: %s\n", error.what());
            requestStop();
        }
    }

    void Application::cliThread()
    {
        try {
            cli::Parser parser;
            cli::filesystem::setup(parser);
            cli::utilities::setup(parser, m_bus, m_stopSource.get_token());
            cli::motor::setup(parser, m_motors, m_stopSource.get_token());
            cli::run(parser, m_stopSource.get_token());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "CLI failed: %s\n", error.what());
        }
        // Closing the console also shuts down the UI and its peer cleanly.
        requestStop();
    }

    void Application::applicationThread()
    {
        {
            std::scoped_lock lock(m_mutex); // Wait until startup has finished.
        }
        try {
            m_motors.run(m_stopSource.get_token());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Motor client failed: %s\n", error.what());
            requestStop();
        }
        // Keep the context alive until bridge shutdown has drained its work.
        m_bridge.stop();
        m_iccContext.stop();
    }
}
