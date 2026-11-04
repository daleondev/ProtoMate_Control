#include "Session.hpp"
#include "Catalog.hpp"
#include "SharedTransport.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
extern char** environ;

namespace simulation
{
    namespace
    {
        volatile sig_atomic_t stopping{};
        void signalStop(int) { stopping = 1; }
    }
    bool stopRequested() noexcept { return stopping != 0; }
    void installSignalHandlers()
    {
        struct sigaction action{};
        action.sa_handler = signalStop;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
    }
    Session::Session()
    {
        for (auto& fd : m_events) {
            fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (fd < 0) {
                for (int owned : m_events)
                    if (owned >= 0)
                        close(owned);
                throw std::runtime_error{ "simulation eventfd" };
            }
        }
    }
    Session::~Session()
    {
        stop();
        for (int fd : m_events)
            if (fd >= 0)
                close(fd);
    }
    void Session::launch(const char* executable, std::span<char*> arguments)
    {
        posix_spawn_file_actions_t actions{};
        if (posix_spawn_file_actions_init(&actions))
            throw std::runtime_error{ "spawn actions" };
        constexpr std::array<const char*, 5> names{ "PROTOMATE_SIM_FD",
                                                    "PROTOMATE_SIM_REQUEST_FD",
                                                    "PROTOMATE_SIM_RESPONSE_FD",
                                                    "PROTOMATE_SIM_INTERRUPT_FD",
                                                    "PROTOMATE_SIM_ACK_FD" };
        std::array<std::string, 5> variables{};
        std::vector<char*> environment{};
        for (auto** entry{ environ }; *entry; ++entry)
            if (!std::string_view{ *entry }.starts_with("PROTOMATE_SIM_"))
                environment.push_back(*entry);
        for (unsigned i{}; i < 5; ++i) {
            const int source{ i == 0 ? m_mapping.fd() : m_events[i - 1] };
            const int target{ static_cast<int>(198 + i) };
            if (posix_spawn_file_actions_adddup2(&actions, source, target)) {
                posix_spawn_file_actions_destroy(&actions);
                throw std::runtime_error{ "spawn descriptor" };
            }
            variables[i] = std::string{ names[i] } + "=" + std::to_string(target);
            environment.push_back(variables[i].data());
        }
        environment.push_back(nullptr);
        std::vector<char*> argv{};
        argv.push_back(const_cast<char*>(executable));
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        argv.push_back(nullptr);
        const int result{ posix_spawn(
          &m_child, executable, &actions, nullptr, argv.data(), environment.data()) };
        posix_spawn_file_actions_destroy(&actions);
        if (result)
            throw std::runtime_error{ "cannot launch controller: " + std::to_string(result) };
    }
    void Session::start()
    {
        if (m_running.exchange(true))
            throw std::logic_error{ "session already started" };
        m_worker = std::jthread{ [this](std::stop_token stop) {
            try {
                work(stop);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "simulation: %s\n", e.what());
                m_child_status = 1;
                m_running = false;
                release(memory().closed, 1U);
            }
        } };
    }
    void Session::stop() noexcept
    {
        release(memory().closed, 1U);
        m_worker.request_stop();
        for (int fd : m_events)
            notify(fd);
        if (m_worker.joinable())
            m_worker.join();
        if (m_child > 0) {
            int status{};
            const auto deadline{ hostNow() + 2'000'000'000ULL };
            pid_t result{};
            do {
                result = waitpid(m_child, &status, WNOHANG);
                if (result == 0)
                    usleep(1000);
            } while (result == 0 && hostNow() < deadline);
            if (result == 0) {
                kill(m_child, SIGTERM);
                const auto grace{ hostNow() + 100'000'000ULL };
                do {
                    result = waitpid(m_child, &status, WNOHANG);
                    if (result == 0)
                        usleep(1000);
                } while (result == 0 && hostNow() < grace);
                if (result == 0) {
                    kill(m_child, SIGKILL);
                    while (waitpid(m_child, &status, 0) < 0 && errno == EINTR) {
                    }
                }
            }
            m_child = -1;
        }
        m_running = false;
    }
    PlantSnapshot Session::snapshot() const
    {
        const std::scoped_lock lock{ m_snapshot_mutex };
        return m_snapshot;
    }
    void Session::work(std::stop_token stop)
    {
        auto& shared{ memory() };
        release(shared.epoch_ns,
                static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch() /
                                           std::chrono::nanoseconds{ 1 }));
        Machine machine{ shared,
                         {},
                         std::getenv("PROTOMATE_MEDIA_DIR") ? std::getenv("PROTOMATE_MEDIA_DIR") : "" };
        pnm::msg::Bus bus{};
        std::ofstream trace{};
        if (const auto* path{ std::getenv("PROTOMATE_TRACE") }) {
            trace.exceptions(std::ios::badbit | std::ios::failbit);
            trace.open(path);
            trace << "time_ns,port,pin,level\n";
            machine.enableTrace();
        }
        const auto drain_trace{ [&] {
            if (!trace.is_open())
                return;
            if (machine.traceOverflow())
                throw std::runtime_error{ "signal trace overflow" };
            for (const auto& event : machine.trace())
                trace << event.time_ns << ',' << char('A' + event.pin / 16) << ',' << event.pin % 16 << ','
                      << event.high << '\n';
            machine.clearTrace();
        } };
        pnm::coro::Context context{};
        SharedTransport transport{ shared, 0, context };
        std::jthread runner{ [&](std::stop_token token) {
            std::stop_callback stop_context{ token, [&] { context.stop(); } };
            context.run();
        } };
        icc::Bridge bridge{ bus,
                            context,
                            transport,
                            { .heartbeat = std::chrono::milliseconds{ 250 },
                              .timeout = std::chrono::seconds{ 10 } } };
        bridge.registerServices<Catalog>(Provider::Machine);
        auto service{ bus.service<^^Catalog::control>().serve([&](const Packet& request) {
            Packet q{ request };
            q.session = shared.session;
            q.sequence = 0;
            // Simulator controls never masquerade as HAL register writes.
            if (q.operation != Operation::State && q.operation != Operation::Fault &&
                q.operation != Operation::Power && q.operation != Operation::Hello)
                return Packet{ .operation = q.operation, .status = EINVAL };
            return machine.execute(q);
        }) };
        bridge.start();
        const auto started{ hostNow() };
        auto next_wall{ started };
        std::uint64_t next_tick{ 1'000'000 }, last_snapshot{};
        bool had_client{};
        while (!stop.stop_requested() && !stopRequested() && !acquire(shared.closed)) {
            const auto wall{ hostNow() };
            release(shared.server_heartbeat, wall);
            for (unsigned batch{}; batch < 64; ++batch) {
                std::array<std::byte, FRAME_BYTES> bytes{};
                std::size_t size{};
                const auto received{ dequeue(shared.registers[0], bytes, size) };
                if (received == QueueResult::Empty)
                    break;
                if (received != QueueResult::Success)
                    throw std::runtime_error{ "corrupt hardware queue" };
                auto request{ decode(std::span{ bytes }.first(size)) };
                const auto encoded{ encode(machine.execute(request)) };
                if (enqueue(shared.registers[1], encoded) != QueueResult::Success)
                    throw std::runtime_error{ "hardware reply queue overflow" };
                notify(m_events[1]);
            }
            service.poll();
            const bool ready{ acquire(shared.client_ready) != 0 };
            had_client |= ready;
            if (m_child > 0) {
                int status{};
                const auto ended{ waitpid(m_child, &status, WNOHANG) };
                if (ended == m_child) {
                    m_child_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                    m_child = -1;
                    break;
                }
            }
            if (!ready && wall - started > 10'000'000'000ULL)
                throw std::runtime_error{ "controller startup timeout" };
            const auto heartbeat{ acquire(shared.client_heartbeat) };
            if (had_client && heartbeat && wall > heartbeat && wall - heartbeat > 1'000'000'000ULL) {
                std::fprintf(stderr,
                             "heartbeat failure at sim=%llu irq=%u ack=%u lease=%d\n",
                             static_cast<unsigned long long>(machine.time()),
                             acquire(shared.irq_sequence),
                             acquire(shared.irq_ack),
                             machine.leased());
                throw std::runtime_error{ "controller heartbeat timeout" };
            }
            const bool outstanding{ acquire(shared.irq_sequence) != acquire(shared.irq_ack) };
            if (ready && !outstanding && !machine.leased() && wall >= next_wall) {
                machine.advance(next_tick - machine.time());
                auto flags{ machine.takeInterrupt() };
                if (machine.time() == next_tick) {
                    flags |= 1U;
                    next_tick += 1'000'000;
                    next_wall = wall + 1'000'000;
                }
                if (flags) {
                    release(shared.irq_flags, flags);
                    release(shared.irq_sequence, acquire(shared.irq_sequence) + 1U);
                    notify(m_events[2]);
                }
            }
            if (wall - last_snapshot >= 10'000'000) {
                auto state{ machine.snapshot() };
                state.published_host_ns = hostNow();
                {
                    const std::scoped_lock lock{ m_snapshot_mutex };
                    m_snapshot = state;
                }
                bus.topic<^^Catalog::state>().publish(state);
                last_snapshot = wall;
            }
            drain_trace();
            // Eventfd wakeups are independent of simulation time, including while a lease is held.
            std::array<pollfd, 2> wakeups{ { { m_events[0], POLLIN, 0 }, { m_events[3], POLLIN, 0 } } };
            if (poll(wakeups.data(), wakeups.size(), 1) > 0)
                for (const auto& wake : wakeups)
                    if (wake.revents & POLLIN)
                        waitNotification(wake.fd, 0);
        }
        machine.disconnect();
        drain_trace();
        bridge.stop(std::chrono::milliseconds{ 100 });
        release(shared.closed, 1U);
        for (int fd : m_events)
            notify(fd);
        m_running = false;
    }
}
