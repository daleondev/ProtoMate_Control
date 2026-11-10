#include "Session.hpp"
#include "Catalog.hpp"
#include "Machine.hpp"
#include "SharedTransport.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace simulation
{
    namespace
    {
        constexpr std::uint64_t TICK_NS{ 1'000'000 }, SNAPSHOT_NS{ 10'000'000 },
          STARTUP_NS{ 10'000'000'000ULL }, HEARTBEAT_NS{ 1'000'000'000ULL };
        constexpr unsigned REGISTER_BATCH{ 64 }, PINS_PER_PORT{ 16 };
        constexpr auto BRIDGE_HEARTBEAT{ std::chrono::milliseconds{ 250 } };
        constexpr auto BRIDGE_TIMEOUT{ std::chrono::seconds{ 10 } };
        constexpr auto BRIDGE_STOP{ std::chrono::milliseconds{ 100 } };
        // Signal handler handoff; the static assertion below requires lock-free atomic operations.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_bool stopping{};
        static_assert(std::atomic_bool::is_always_lock_free);
        void signal_stop(int /*unused*/) { stopping.store(true, std::memory_order_relaxed); }
        void drain_trace(std::ofstream& trace, Machine& machine)
        {
            if (!trace.is_open()) {
                return;
            }
            if (machine.traceOverflow()) {
                throw std::runtime_error{ "signal trace overflow" };
            }
            for (const auto& event : machine.trace()) {
                trace << event.time_ns << ',' << char('A' + (event.pin / PINS_PER_PORT)) << ','
                      << event.pin % PINS_PER_PORT << ',' << event.high << '\n';
            }
            machine.clearTrace();
        }
    }
    bool stop_requested() noexcept { return stopping.load(std::memory_order_relaxed); }
    void install_signal_handlers()
    {
        struct sigaction action{};
        action.sa_handler = signal_stop;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
    }
    Session::Session(SessionOptions options)
      : m_options{ std::move(options) }
      // CAD collision queries can exceed the live hardware-link timeout under
      // instrumentation. Compute immutable geometry before launching the child.
      , m_configuration{ Machine::configuration(m_options.profile) }
    {
        m_options.profile.validate();
        for (auto& fd : m_events) {
            fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (fd < 0) {
                for (int owned : m_events) {
                    if (owned >= 0) {
                        close(owned);
                    }
                }
                throw std::runtime_error{ "simulation eventfd" };
            }
        }
    }
    Session::~Session()
    {
        stop();
        for (int fd : m_events) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }
    void Session::launch(int executable_fd, std::span<char*> arguments)
    {
        if (acquire(memory().closed)) {
            throw std::logic_error{ "session is closed" };
        }
        m_process.launch(
          executable_fd, arguments, { m_mapping.fd(), m_events[0], m_events[1], m_events[2], m_events[3] });
    }
    void Session::start()
    {
        if (!m_process.running()) {
            throw std::logic_error{ "launch controller before simulation worker" };
        }
        if (m_running.exchange(true)) {
            throw std::logic_error{ "session already started" };
        }
        m_worker = std::jthread{ [this](const std::stop_token& stop) {
            try {
                work(stop);
            } catch (const std::exception& e) {
                // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                std::fprintf(stderr, "simulation: %s\n", e.what());
                m_childStatus = 1;
                release(memory().closed, 1U);
            }
            release(memory().closed, 1U);
            for (int fd : m_events) {
                notify(fd);
            }
            const int status{ m_process.finish() };
            if (!m_childStatus.load()) {
                m_childStatus = status;
            }
            m_running = false;
        } };
    }
    void Session::stop() noexcept
    {
        release(memory().closed, 1U);
        m_worker.request_stop();
        for (int fd : m_events) {
            notify(fd);
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
        const int status{ m_process.finish() };
        if (!m_childStatus.load()) {
            m_childStatus = status;
        }
        m_running = false;
    }
    PlantSnapshot Session::snapshot() const
    {
        const std::scoped_lock lock{ m_snapshotMutex };
        return m_snapshot;
    }
    void Session::work(const std::stop_token& stop)
    {
        auto& shared{ memory() };
        release(shared.epoch_ns,
                static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch() /
                                           std::chrono::nanoseconds{ 1 }));
        Machine machine{ shared, m_options.profile, m_options.media_directory, m_configuration };
        try {
            execute(machine, stop);
        } catch (...) {
            machine.disconnect();
            publishSnapshot(machine);
            throw;
        }
        machine.disconnect();
        publishSnapshot(machine);
    }
    void Session::publishSnapshot(const Machine& machine)
    {
        auto state{ machine.snapshot() };
        state.published_host_ns = host_now();
        const std::scoped_lock lock{ m_snapshotMutex };
        m_snapshot = state;
    }
    void Session::serviceRegisters(Machine& machine)
    {
        auto& shared{ memory() };
        for (unsigned batch{}; batch < REGISTER_BATCH; ++batch) {
            std::array<std::byte, FRAME_BYTES> bytes{};
            std::size_t size{};
            const auto received{ dequeue(shared.registers[0], bytes, size) };
            if (received == QueueResult::Empty) {
                break;
            }
            if (received != QueueResult::Success) {
                throw std::runtime_error{ "corrupt hardware queue" };
            }
            auto request{ decode(std::span{ bytes }.first(size)) };
            const auto encoded{ encode(machine.execute(request)) };
            if (enqueue(shared.registers[1], encoded) != QueueResult::Success) {
                throw std::runtime_error{ "hardware reply queue overflow" };
            }
            notify(m_events[1]);
        }
    }
    void Session::advanceClock(Machine& machine, ClockState& clock, std::uint64_t wall, bool ready)
    {
        auto& shared{ memory() };
        const bool outstanding{ acquire(shared.irq_sequence) != acquire(shared.irq_ack) };
        // ISR acknowledgement alone does not mean the woken application
        // workers have run. Advancing again while they are still runnable
        // can starve low-priority homing under instrumentation. Time advances
        // only after the scheduler has drained this interrupt's ready work.
        const bool idle{ acquire(shared.idle_epoch) == acquire(shared.irq_sequence) + 1U };
        if (ready && !outstanding && idle && !machine.leased() && wall >= clock.next_wall) {
            machine.advance(clock.next_tick - machine.time());
            auto flags{ machine.takeInterrupt() };
            if (machine.time() == clock.next_tick) {
                flags |= 1U;
                clock.next_tick += TICK_NS;
                clock.next_wall = wall + TICK_NS;
            }
            if (flags) {
                release(shared.irq_flags, flags);
                release(shared.irq_sequence, acquire(shared.irq_sequence) + 1U);
                notify(m_events[2]);
            }
        }
    }
    void Session::checkPeer(const Machine& machine, PeerState state)
    {
        auto& shared{ memory() };
        if (!state.ready && state.wall - state.started > STARTUP_NS) {
            throw std::runtime_error{ "controller startup timeout" };
        }
        const auto heartbeat{ acquire(shared.client_heartbeat) };
        if (state.had_client && heartbeat && state.wall > heartbeat &&
            state.wall - heartbeat > HEARTBEAT_NS) {
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
            std::fprintf(stderr,
                         "heartbeat failure at sim=%llu irq=%u ack=%u lease=%d\n",
                         static_cast<unsigned long long>(machine.time()),
                         acquire(shared.irq_sequence),
                         acquire(shared.irq_ack),
                         machine.leased());
            throw std::runtime_error{ "controller heartbeat timeout" };
        }
    }
    void Session::execute(Machine& machine, const std::stop_token& stop)
    {
        auto& shared{ memory() };
        pnm::msg::Bus bus{};
        std::ofstream trace{};
        if (!m_options.trace_path.empty()) {
            trace.exceptions(std::ios::badbit | std::ios::failbit);
            trace.open(m_options.trace_path);
            trace << "time_ns,port,pin,level\n";
            machine.enableTrace();
        }
        pnm::coro::Context context{};
        SharedTransport transport{ shared, 0, context };
        std::jthread runner{ [&](const std::stop_token& token) {
            std::stop_callback stop_context{ token, [&] { context.stop(); } };
            context.run();
        } };
        icc::Bridge bridge{
            bus, context, transport, { .heartbeat = BRIDGE_HEARTBEAT, .timeout = BRIDGE_TIMEOUT }
        };
        bridge.registerServices<Catalog>(Provider::Machine);
        auto service{ bus.service<^^Catalog::control>().serve([&](const Packet& request) {
            Packet q{ request };
            q.session = shared.session;
            q.sequence = 0;
            // Simulator controls never masquerade as HAL register writes.
            if (q.operation != Operation::State && q.operation != Operation::Configuration &&
                q.operation != Operation::Fault && q.operation != Operation::Power &&
                q.operation != Operation::Hello) {
                return Packet{ .operation = q.operation, .status = EINVAL };
            }
            return machine.execute(q);
        }) };
        bridge.start();
        const auto started{ host_now() };
        ClockState clock{ .next_wall = started, .next_tick = TICK_NS };
        std::uint64_t last_snapshot{};
        bool had_client{};
        while (!stop.stop_requested() && !stop_requested() && !acquire(shared.closed)) {
            const auto wall{ host_now() };
            release(shared.server_heartbeat, wall);
            serviceRegisters(machine);
            service.poll();
            const bool ready{ acquire(shared.client_ready) != 0 };
            had_client |= ready;
            if (const auto status{ m_process.poll() }) {
                m_childStatus = *status;
                break;
            }
            checkPeer(machine,
                      { .wall = wall, .started = started, .ready = ready, .had_client = had_client });
            advanceClock(machine, clock, wall, ready);
            if (wall - last_snapshot >= SNAPSHOT_NS) {
                auto state{ machine.snapshot() };
                state.published_host_ns = host_now();
                {
                    const std::scoped_lock lock{ m_snapshotMutex };
                    m_snapshot = state;
                }
                bus.topic<^^Catalog::state>().publish(state);
                last_snapshot = wall;
            }
            drain_trace(trace, machine);
            // Eventfd wakeups are independent of simulation time, including while a lease is held.
            std::array<pollfd, 2> wakeups{ { { .fd = m_events[0], .events = POLLIN, .revents = 0 },
                                             { .fd = m_events[3], .events = POLLIN, .revents = 0 } } };
            if (poll(wakeups.data(), wakeups.size(), 1) > 0) {
                for (const auto& wake : wakeups) {
                    if (wake.revents & POLLIN) {
                        wait_notification(wake.fd, std::chrono::milliseconds{ 0 });
                    }
                }
            }
        }
        machine.disconnect();
        drain_trace(trace, machine);
        if (trace.is_open()) {
            trace.flush();
        }
        bridge.stop(BRIDGE_STOP);
        release(shared.closed, 1U);
        for (int fd : m_events) {
            notify(fd);
        }
    }
}
