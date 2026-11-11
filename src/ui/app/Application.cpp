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
#include "ui/cli/profiling.hpp"
#include "ui/cli/robot.hpp"
#include "ui/cli/state.hpp"
#include "ui/cli/utilities.hpp"
#include "ui/cli/network.hpp"
#include "ui/opcua/util/Storage.hpp"
#include "ui/logging/Sink.hpp"

#include <cstdio>
#include <stdexcept>

#include "pneumo/logging.hpp"

namespace ui
{
    namespace
    {
        constexpr unsigned LOG_PRIORITY{ 20 };
        constexpr std::size_t LOG_STACK_BYTES{ 8192 };
    }
    namespace
    {
        void report_failure(const char* worker) noexcept
        {
            try {
                throw;
            } catch (const std::exception& error) {
                // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                std::fprintf(stderr, "UI %s failed: %s\n", worker, error.what());
            } catch (...) {
                // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                std::fprintf(stderr, "UI %s failed: unknown exception\n", worker);
            }
        }
    }
#if defined(CORE_CM7)
    // A dedicated private stack preserves AXI heap for ICC/CLI. The linker
    // checks the DTCM budget; no runtime objects live in the SDRAM POD arena.
    __attribute__((section(".dtcm_bss"))) Application::PlannerStack Application::s_plannerStack{};
    __attribute__((section(".dtcm_bss"))) Application::NetworkStacks Application::s_networkStacks{};
#endif
    Application::Application(icc::ITransport& transport)
      : m_bridge{ m_bus, m_iccContext, transport }
      , m_systemState{ hal::core::create_error_led(), hal::core::create_status_led() }
      , m_ioState{ m_bus.topic<^^icc::Topics::io_controller_state>().subscribe(
          [this](app::State state) { m_systemState.receiveIo(state); },
          pnm::msg::SubscriptionOptions::latestOnly()) }
      , m_logs{ m_bus, cli::write_log }
      , m_motors{ m_bus }
      , m_robot{ m_bus, m_motors }
      , m_services{ m_bus, m_motors, m_robot, m_systemState }
      , m_guiBackend{ m_services, m_motors }
    {
        m_gui.backend(m_guiBackend);
        m_guiBackend.networking(m_network, m_opcua);
        m_services.networkHook([this] {return m_network.diagnostics();});
        m_services.graphicsHooks([this] {
            const auto gui{m_gui.status()}; const auto scene{m_gui.sceneStatus()};
            return services::Graphics{.available = gui.state == gui::State::Running,
                .brightness = gui.brightness, .frames = gui.frames, .presented = gui.presented,
                .underruns = gui.underruns, .dma_errors = gui.dma_errors, .render_failures = gui.render_failures,
                .queue_overflow = gui.queue_overflow, .maximum_render_us = gui.maximum_render_us,
                .scene_presented = scene.presented, .scene_latency_us = scene.last_latency_us};
        }, [this](unsigned percent) {return m_gui.backlight(percent);});
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
                // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                std::fprintf(stderr, "UI ICC failed: reason=%u\n", static_cast<unsigned>(status.reason));
                if (const auto failure{ m_bridge.failure() }) {
                    if (const auto* exception{ std::get_if<std::exception_ptr>(&*failure) }) {
                        try {
                            std::rethrow_exception(*exception);
                        } catch (const std::exception& error) {
                            m_systemState.setFailure(error.what());
                            m_guiBackend.connection(status, error.what());
                            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                            std::fprintf(stderr, "ICC exception: %s\n", error.what());
                        } catch (...) {
                            m_systemState.setFailure("non-standard exception");
                            m_guiBackend.connection(status, "non-standard exception");
                            report_failure("ICC");
                        }
                    }
                }
            }
            m_motors.setConnected(status.state == icc::ConnectionState::Connected);
            m_robot.setConnected(status.state == icc::ConnectionState::Connected);
            // Keep the fault display/CLI alive. M4 independently enforces its
            // loss-of-owner stop; losing ICC is not proof that hardware stopped.
        });

        runtime::thread::publish_attributes(
          { .name = "ui.logging", .priority = LOG_PRIORITY, .stack_size = LOG_STACK_BYTES });
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
            if (!opcua::initialize_storage()) throw std::runtime_error("OPC UA SDRAM integrity/alignment test failed");
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
                report_failure("state publication");
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
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
            std::fprintf(stderr, "Log subscriber failed: %s\n", error.what());
            requestStop();
        } catch (...) {
            report_failure("log subscriber");
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
            simulation::setup_commands(parser);
#endif
            cli::filesystem::setup(parser);
            cli::display::setup(parser, m_gui);
            cli::state::setup(parser, m_systemState);
            cli::network::setup(parser, m_network, m_opcua, m_services, m_stopSource.get_token());
            cli::utilities::setup(parser, m_bus, m_stopSource.get_token());
            cli::profiling::setup(parser, m_bus, m_stopSource.get_token());
            cli::motor::setup(parser, m_motors, m_stopSource.get_token());
            cli::robot::setup(parser, m_robot);
            cli::motion::setup(parser, m_motors, m_robot, m_stopSource.get_token());
            cli::run(parser, m_stopSource.get_token());
        } catch (const std::exception& error) {
            m_systemState.setLocal(app::State::Fault);
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
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
            report_failure("planner");
            m_systemState.setLocal(app::State::Fault);
            requestStop();
        }
    }

    void Application::opcuaThread()
    {
        { std::scoped_lock lock{ m_mutex }; }
        try { m_opcua.run(m_stopSource.get_token()); }
        catch (...) {
            m_opcua.failed();
            m_services.message("OPC UA worker failed; remote admission revoked; local controls remain available", services::Severity::Error);
            report_failure("OPC UA");
        }
    }
    void Application::networkThread()
    {
        { std::scoped_lock lock{ m_mutex }; }
        try { m_network.run(m_stopSource.get_token()); }
        catch (...) {
            m_network.failed();
            m_services.remotePermission(false);
            m_services.message("Network worker failed; remote admission revoked; local controls remain available", services::Severity::Error);
            report_failure("network");
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
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
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
