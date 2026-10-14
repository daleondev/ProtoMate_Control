#include "Application.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"
#include "ui/cli/Console.hpp"
#include "hal/console.hpp"
#include "hal/core/core.hpp"
#include "ui/cli/display.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/filesystem.hpp"
#include "ui/cli/motor.hpp"
#include "ui/cli/state.hpp"
#include "ui/cli/utilities.hpp"
#include "ui/logging/Sink.hpp"

#include <cstdio>
#include <stdexcept>

#include "pneumo/logging.hpp"

namespace ui
{
    Application::Application(icc::ITransport& transport)
      : m_bridge(m_bus, m_iccContext, transport)
      , m_systemState{ hal::core::create_error_led(), hal::core::create_status_led() }
      , m_ioState{ m_bus.topic<^^icc::Topics::io_controller_state>().subscribe(
          [this](app::State state) { m_systemState.receiveIo(state); },
          pnm::msg::SubscriptionOptions::latestOnly()) }
      , m_logs{ m_bus, cli::writeLog }
      , m_motors{ m_bus }
    {
        hal::console::initialize();
        m_bridge.registerTopics<icc::Topics>(icc::TopicPublisher::UI);
        m_bridge.registerServices<icc::Services>(icc::ServiceProvider::UI);
        m_bridge.registerActions<icc::Actions>(icc::ActionProvider::UI);
        m_bridge.setConnectionCallback([this](icc::ConnectionStatus status) {
            m_systemState.setConnection(status);
            if (status.state == icc::ConnectionState::Failed) { m_systemState.setLocal(app::State::Fault); }
            m_motors.setConnected(status.state == icc::ConnectionState::Connected);
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
            m_systemState.setLocal(app::State::Ready);
        } catch (...) {
            m_systemState.setLocal(app::State::Fault);
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
            m_systemState.setLocal(app::State::Stopping);
        }
        m_stopSource.request_stop();
    }

    void Application::join()
    {
        m_threads.join();
        if (m_stopSource.stop_requested()) { m_systemState.setLocal(app::State::Stopped); }
    }

    icc::ConnectionStatus Application::connectionStatus() const { return m_bridge.connectionStatus(); }

    void Application::installLogSink()
    {
        if (!pnm::log::set_default_sink(
              pnm::log::Level::Trace, pnm::log::Level::Critical, std::make_shared<logging::Sink>())) {
            throw std::runtime_error("failed to configure UI logging");
        }
    }

    void Application::refreshState()
    {
        try {
            m_ioState.pollSome(1);
            m_systemState.refresh();
            if (!m_stopSource.stop_requested()) {
                m_stateTimer = m_iccContext.scheduleAt(
                  std::chrono::steady_clock::now() + app::STATE_PERIOD, [this] { refreshState(); });
            }
        } catch (...) {
            if (!m_stopSource.stop_requested()) {
                m_systemState.setLocal(app::State::Fault);
                requestStop();
            }
        }
    }

    void Application::iccThread()
    {
        refreshState();
        m_iccContext.run();
        m_stateTimer.cancel();
    }

    void Application::logThread()
    {
        try {
            m_logs.run(m_stopSource.get_token());
        } catch (const std::exception& error) {
            m_systemState.setLocal(app::State::Fault);
            std::fprintf(stderr, "Log subscriber failed: %s\n", error.what());
            requestStop();
        } catch (...) {
            m_systemState.setLocal(app::State::Fault);
            requestStop();
        }
    }

    void Application::cliThread()
    {
        try {
            cli::Parser parser;
            cli::filesystem::setup(parser);
            cli::display::setup(parser);
            cli::state::setup(parser, m_systemState);
            cli::utilities::setup(parser, m_bus, m_stopSource.get_token());
            cli::motor::setup(parser, m_motors, m_stopSource.get_token());
            cli::run(parser, m_stopSource.get_token());
        } catch (const std::exception& error) {
            m_systemState.setLocal(app::State::Fault);
            std::fprintf(stderr, "CLI failed: %s\n", error.what());
        } catch (...) {
            m_systemState.setLocal(app::State::Fault);
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
            m_systemState.setLocal(app::State::Fault);
            std::fprintf(stderr, "Motor client failed: %s\n", error.what());
            requestStop();
        } catch (...) {
            m_systemState.setLocal(app::State::Fault);
            requestStop();
        }
        // Keep the context alive until bridge shutdown has drained its work.
        m_bridge.stop();
        m_iccContext.stop();
    }
}
