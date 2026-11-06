#ifdef HAL_PLATFORM_LINUX
#include "simulation/ControllerLink.hpp"
#endif
#include "Application.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"
#include "hal/console.hpp"
#include "hal/core/core.hpp"
#include "ui/cli/Console.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/display.hpp"
#include "ui/cli/filesystem.hpp"
#include "ui/cli/motion.hpp"
#include "ui/cli/motor.hpp"
#include "ui/cli/robot.hpp"
#include "ui/cli/state.hpp"
#include "ui/cli/utilities.hpp"
#include "ui/logging/Sink.hpp"

#include <cstdio>
#include <stdexcept>

#include "pneumo/logging.hpp"

namespace ui
{
    namespace
    {
        void reportFailure(const char* worker) noexcept
        {
            try {
                throw;
            } catch (const std::exception& error) {
                std::fprintf(stderr, "UI %s failed: %s\n", worker, error.what());
            } catch (...) {
                std::fprintf(stderr, "UI %s failed: unknown exception\n", worker);
            }
        }
    }
#if defined(CORE_CM7)
    // A dedicated private stack preserves AXI heap for ICC/CLI. The linker
    // checks the DTCM budget; no runtime objects live in the SDRAM POD arena.
    alignas(32) __attribute__((section(".dtcm_bss"))) std::byte Application::s_plannerStack[24UZ * 1024UZ];
#endif
    Application::Application(icc::ITransport& transport)
      : m_bridge{ m_bus, m_iccContext, transport }
      , m_systemState{ hal::core::create_error_led(), hal::core::create_status_led() }
      , m_ioState{ m_bus.topic<^^icc::Topics::io_controller_state>().subscribe(
          [this](app::State state) { m_systemState.receiveIo(state); },
          pnm::msg::SubscriptionOptions::latestOnly()) }
      , m_logs{ m_bus, cli::writeLog }
      , m_motors{ m_bus }
      , m_robot{ m_bus, m_motors }
      , m_guiBackend{ m_bus, m_motors, m_robot, m_systemState }
    {
        m_gui.backend(m_guiBackend);
        hal::console::initialize();
        m_bridge.registerTopics<icc::Topics>(icc::TopicPublisher::UI);
        m_bridge.registerServices<icc::Services>(icc::ServiceProvider::UI);
        m_bridge.registerActions<icc::Actions>(icc::ActionProvider::UI);
        m_bridge.setConnectionCallback([this](icc::ConnectionStatus status) {
            m_systemState.setConnection(status);
            m_guiBackend.connection(status);
            if (status.state == icc::ConnectionState::Failed ||
                (status.reason == icc::ConnectionReason::PeerStopped && !m_stopSource.stop_requested())) {
                m_systemState.setLocal(app::State::Fault);
                std::fprintf(stderr, "UI ICC failed: reason=%u\n", static_cast<unsigned>(status.reason));
                if (const auto failure = m_bridge.failure()) {
                    if (const auto* exception = std::get_if<std::exception_ptr>(&*failure)) {
                        try {
                            std::rethrow_exception(*exception);
                        } catch (const std::exception& error) {
                            m_systemState.setFailure(error.what());
                            m_guiBackend.connection(status, error.what());
                            std::fprintf(stderr, "ICC exception: %s\n", error.what());
                        } catch (...) {
                            m_systemState.setFailure("non-standard exception");
                            m_guiBackend.connection(status, "non-standard exception");
                            reportFailure("ICC");
                        }
                    }
                }
            }
            m_motors.setConnected(status.state == icc::ConnectionState::Connected);
            m_robot.setConnected(status.state == icc::ConnectionState::Connected);
            // Keep the fault display/CLI alive. M4 independently enforces its
            // loss-of-owner stop; losing ICC is not proof that hardware stopped.
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
        std::unique_lock lock{ m_mutex };
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
            std::scoped_lock lock{ m_mutex };
            m_stopping = true;
            m_systemState.setLocal(app::State::Stopping);
        }
        m_stopSource.request_stop();
    }

    void Application::join()
    {
        m_threads.join();
        if (m_stopSource.stop_requested()) {
            m_systemState.setLocal(app::State::Stopped);
        }
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
                m_stateTimer = m_iccContext.scheduleAt(std::chrono::steady_clock::now() + app::STATE_PERIOD,
                                                       [this] { refreshState(); });
            }
        } catch (...) {
            if (!m_stopSource.stop_requested()) {
                reportFailure("state publication");
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
            reportFailure("log subscriber");
            m_systemState.setLocal(app::State::Fault);
            requestStop();
        }
    }

    void Application::guiThread()
    {
        // start() holds this lock while creating every worker and starting ICC.
        // Do not let the higher-priority GUI's initialization preempt that setup.
        {
            std::scoped_lock lock{ m_mutex };
        }
        if (m_gui.run(m_stopSource.get_token())) {
            requestStop();
        }
    }
    void Application::renderThread() { m_gui.render3d(m_stopSource.get_token()); }
    void Application::touchThread() { m_gui.acquireTouch(m_stopSource.get_token()); }

    void Application::cliThread()
    {
        try {
            cli::Parser parser{};
#ifdef HAL_PLATFORM_LINUX
            simulation::setupCommands(parser);
#endif
            cli::filesystem::setup(parser);
            cli::display::setup(parser, m_gui);
            cli::state::setup(parser, m_systemState);
            cli::utilities::setup(parser, m_bus, m_stopSource.get_token());
            cli::motor::setup(parser, m_motors, m_stopSource.get_token());
            cli::robot::setup(parser, m_robot);
            cli::motion::setup(parser, m_motors, m_robot, m_stopSource.get_token());
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

    void Application::plannerThread()
    {
        try {
            m_robot.run(m_stopSource.get_token());
        } catch (...) {
            reportFailure("planner");
            m_systemState.setLocal(app::State::Fault);
            requestStop();
        }
    }

    void Application::applicationThread()
    {
        {
            std::scoped_lock lock{ m_mutex }; // Wait until startup has finished.
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
