#include "Client.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <string_view>
#include <tx_api.h>
#include <unistd.h>

namespace simulation::client
{
    namespace
    {
        constexpr std::size_t TIMER_CALLBACK_COUNT{ TIMER_COUNT + PWM_COUNT }, DEFERRED_CAPACITY{ 64 };
        constexpr unsigned RESPONSE_WAIT_MS{ 10 }, PERIPHERAL_IRQ_MASK{ 24U };
        constexpr std::uint64_t PEER_TIMEOUT_NS{ 1'000'000'000ULL }, STARTUP_TIMEOUT_NS{ 10'000'000'000ULL };
        // Read by the native scheduler while its ThreadX mutex is held. Never
        // lazily initialize the connection or call a ThreadX API from that hook.
        // Native scheduling callback bridge; published atomically for one controller process.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic<SharedMemory*> scheduler_memory{};
        static_assert(decltype(scheduler_memory)::is_always_lock_free);
        struct Connection
        {
            std::unique_ptr<Mapping> mapping;
            int request_fd{ -1 }, response_fd{ -1 }, interrupt_fd{ -1 }, acknowledged_fd{ -1 };
            std::uint64_t sequence{};
            std::atomic_bool failed{};
            unsigned depth{};
            bool isr{};
            std::array<hal::GpioInput*, GPIO_COUNT> inputs{};
            std::array<void*, TIMER_CALLBACK_COUNT> timers{};
            std::array<void (*)(void*), TIMER_CALLBACK_COUNT> timer_callbacks{};
            std::array<Packet, DEFERRED_CAPACITY> deferred{};
            unsigned deferred_count{}, bulk_used{};
            std::uint64_t started{ host_now() };
            std::function<void()> step_callback;
            Connection()
            {
                const auto* value{ std::getenv("PROTOMATE_SIM_FD") };
                if (!value) {
                    return;
                }
                try {
                    const auto descriptor{ [](const char* name) {
                        const char* text{ std::getenv(name) };
                        if (!text) {
                            throw std::invalid_argument{ "missing simulator descriptor" };
                        }
                        const std::string_view view{ text };
                        int fd{};
                        const auto parsed{ std::from_chars(view.data(), view.data() + view.size(), fd) };
                        if (parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size() || fd < 3 ||
                            // POSIX/Linux variadic ABI with the argument types required by this operation.
                            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                            fcntl(fd, F_GETFD) < 0) {
                            throw std::invalid_argument{ "invalid simulator descriptor" };
                        }
                        return fd;
                    } };
                    const std::array<int, DESCRIPTOR_COUNT> fds{ descriptor("PROTOMATE_SIM_FD"),
                                                                 descriptor("PROTOMATE_SIM_REQUEST_FD"),
                                                                 descriptor("PROTOMATE_SIM_RESPONSE_FD"),
                                                                 descriptor("PROTOMATE_SIM_INTERRUPT_FD"),
                                                                 descriptor("PROTOMATE_SIM_ACK_FD") };
                    for (unsigned i{}; i < fds.size(); ++i) {
                        for (unsigned j{}; j < i; ++j) {
                            if (fds[i] == fds[j]) {
                                throw std::invalid_argument{ "aliased simulator descriptors" };
                            }
                        }
                        // POSIX/Linux variadic ABI with the argument types required by this operation.
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
                            throw std::runtime_error{ "simulator descriptor flags" };
                        }
                    }
                    auto checked{ std::make_unique<Mapping>(fds[0]) };
                    if (acquire(checked->memory().closed) || acquire(checked->memory().client_ready)) {
                        throw std::invalid_argument{ "closed or already claimed simulator session" };
                    }
                    request_fd = fds[1];
                    response_fd = fds[2];
                    interrupt_fd = fds[3];
                    acknowledged_fd = fds[4];
                    mapping = std::move(checked);
                    close(fds[0]);
                    scheduler_memory.store(&mapping->memory(), std::memory_order_release);
                    release(mapping->memory().client_heartbeat, host_now());
                    release(mapping->memory().client_ready, 1U);
                } catch (...) {
                    failed.store(true);
                }
            }
        };
        Connection& connection()
        {
            // ThreadX's interrupt pthread and cached HAL factories live until process
            // exit, including static teardown. Keep their mailbox alive for the
            // same lifetime; the OS releases this one bounded allocation/mapping.
            static Connection* const result{ new Connection{} };
            return *result;
        }
        void fault() noexcept
        {
            auto& c{ connection() };
            if (!c.failed.exchange(true)) {
                std::fputs("simulation: hardware connection lost; execution counts are unknown\n", stderr);
            }
            if (c.mapping) {
                release(c.mapping->memory().closed, 1U);
            }
        }
    }
    void scheduler_idle() noexcept
    {
        if (auto* shared{ scheduler_memory.load(std::memory_order_acquire) }) {
            const auto sequence{ acquire(shared->irq_sequence) };
            if (sequence == acquire(shared->irq_ack)) {
                release(shared->idle_epoch, sequence + 1U);
            }
        }
    }
    bool connected() noexcept { return connection().mapping != nullptr; }
    bool failed() noexcept { return connection().failed.load(); }
    SharedMemory* memory() noexcept { return connected() ? &connection().mapping->memory() : nullptr; }
    bool in_interrupt() noexcept { return connection().isr; }
    std::uint64_t now() noexcept
    {
        auto* m{ memory() };
        return m ? acquire(m->time_ns) : 0;
    }
    Packet call(Packet request) noexcept
    {
        auto& c{ connection() };
        Packet result{ request };
        result.status = ENOTCONN;
        if (!c.mapping || c.failed) {
            return result;
        }
        if (c.isr) {
            if (c.deferred_count == c.deferred.size()) {
                fault();
                return result;
            }
            c.deferred[c.deferred_count++] = request;
            result.status = 0;
            return result;
        }
        auto& m{ c.mapping->memory() };
        release(m.client_heartbeat, host_now());
        request.sequence = ++c.sequence;
        request.session = m.session;
        try {
            auto frame{ encode(request) };
            const auto deadline{ host_now() + PEER_TIMEOUT_NS };
            for (;;) {
                const auto queued{ enqueue(m.registers[0], frame) };
                if (queued == QueueResult::Success) {
                    break;
                }
                if (queued != QueueResult::Full || acquire(m.closed) || host_now() >= deadline) {
                    fault();
                    return result;
                }
                wait_notification(c.response_fd, std::chrono::milliseconds{ 1 });
            }
            notify(c.request_fd);
            for (;;) {
                std::array<std::byte, FRAME_BYTES> bytes{};
                std::size_t size{};
                const auto state{ dequeue(m.registers[1], bytes, size) };
                if (state == QueueResult::Success) {
                    auto reply{ decode(std::span{ bytes }.first(size)) };
                    if (reply.session != m.session || reply.sequence != request.sequence ||
                        reply.operation != request.operation) {
                        fault();
                        return result;
                    }
                    return reply;
                }
                if (state != QueueResult::Empty) {
                    fault();
                    return result;
                }
                if (acquire(m.closed) || host_now() >= deadline) {
                    fault();
                    return result;
                }
                wait_notification(c.response_fd, std::chrono::milliseconds{ RESPONSE_WAIT_MS });
            }
        } catch (...) {
            fault();
            return result;
        }
    }
    bool publish_bank(unsigned axis,
                      unsigned bank,
                      std::span<const std::byte> bytes,
                      std::optional<std::uint32_t> cut) noexcept
    {
        static_assert(sizeof(hal::util::StepBuffer) <= BULK_BYTES);
        auto& c{ connection() };
        if (!c.mapping || c.failed || bytes.size() != sizeof(hal::util::StepBuffer)) {
            return false;
        }
        if (c.isr && c.bulk_used == BULK_COUNT) {
            fault();
            return false;
        }
        // Deferred writes need distinct immutable slots until the entire IRQ
        // batch is acknowledged. Reusing axis*2+bank invalidated earlier writes.
        const unsigned slot{ c.isr ? c.bulk_used++ : 0 };
        auto& shared{ c.mapping->memory() };
        std::ranges::copy(bytes, shared.bulk[slot].begin());
        const auto generation{ acquire(shared.bulk_generation[slot]) + 1U };
        release(shared.bulk_generation[slot], generation);
        return command(cut ? Operation::PublishFuture : Operation::Publish,
                       { axis, bank, slot, generation, checksum(bytes), cut.value_or(0) })
                 .status == 0;
    }
    Packet command(Operation operation, std::array<std::uint64_t, ARGUMENT_COUNT> arguments) noexcept
    {
        return call(Packet{ .operation = operation, .arguments = arguments });
    }
    hal::util::StepSample sample() noexcept
    {
        hal::util::StepSample result{};
        auto* m{ memory() };
        if (!m || failed()) {
            result.error = true;
            return result;
        }
        if (!in_interrupt()) {
            command(Operation::Sample);
        }
        result.tick = acquire(m->step_sample[0]);
        result.running = acquire(m->step_sample[1]);
        result.error = acquire(m->step_sample[2]) || failed();
        for (unsigned i{}; i < 3; ++i) {
            const auto o{ step_wire::HEADER_WORDS + (i * step_wire::CHANNEL_WORDS) };
            auto& c{ result.channels[i] };
            c.tick = acquire(m->step_sample[o]);
            c.compare = acquire(m->step_sample[o + 1]);
            c.remaining = acquire(m->step_sample[o + 2]);
            c.target = static_cast<std::uint8_t>(acquire(m->step_sample[o + 3]));
            c.transfer_complete = acquire(m->step_sample[o + 4]);
            c.compare_pending = acquire(m->step_sample[o + step_wire::PENDING_FIELD]);
            c.high = acquire(m->step_sample[o + step_wire::HIGH_FIELD]);
        }
        return result;
    }
    void set_step_callback(std::function<void()> callback)
    {
        connection().step_callback = std::move(callback);
    }
    void register_timer(unsigned index, void* object, void (*callback)(void*)) noexcept
    {
        connection().timers.at(index) = object;
        connection().timer_callbacks.at(index) = callback;
    }
    void dispatch_timers() noexcept
    {
        auto& c{ connection() };
        for (unsigned i{}; i < c.timers.size(); ++i) {
            if (c.timers[i]) {
                c.timer_callbacks[i](c.timers[i]);
            }
        }
    }
    void register_input(unsigned pin, hal::GpioInput* input) noexcept { connection().inputs.at(pin) = input; }
    void dispatch_inputs() noexcept
    {
        auto* m{ memory() };
        if (!m) {
            return;
        }
        for (unsigned index{}; index < m->gpio.size(); ++index) {
            auto* input{ connection().inputs[index] };
            if (input) {
                input->setSimulatedLevel(acquire(m->gpio[index]) ? hal::gpio::Level::High
                                                                 : hal::gpio::Level::Low);
            }
        }
    }
    void enter() noexcept
    {
        auto& c{ connection() };
        if (c.mapping && c.depth++ == 0 && !c.isr) {
            command(Operation::Begin);
        }
    }
    void leave() noexcept
    {
        auto& c{ connection() };
        if (!c.mapping || c.depth == 0) {
            return;
        }
        --c.depth;
        if (c.depth == 0 && !c.isr) {
            command(Operation::End);
        }
    }
    Transaction::Transaction() noexcept
      : m_active{ connected() }
    {
        if (m_active) {
            m_posture = tx_interrupt_control(TX_INT_DISABLE);
            enter();
        }
    }
    Transaction::~Transaction()
    {
        if (m_active) {
            leave();
            tx_interrupt_control(m_posture);
        }
    }
}
extern "C" void protomate_sim_scheduler_idle() { simulation::client::scheduler_idle(); }
extern "C" int protomate_sim_wait_interrupt()
{
    using namespace simulation;
    auto& c{ client::connection() };
    if (!c.mapping) {
        return -1;
    }
    auto& m{ c.mapping->memory() };
    for (;;) {
        release(m.client_heartbeat, host_now());
        if (acquire(m.closed) || c.failed) {
            return -1;
        }
        if (acquire(m.irq_sequence) != acquire(m.irq_ack)) {
            return static_cast<int>(acquire(m.irq_flags));
        }
        const auto last{ acquire(m.server_heartbeat) };
        const auto wall{ host_now() };
        if ((last && wall > last && wall - last > client::PEER_TIMEOUT_NS) ||
            (!last && wall - c.started > client::STARTUP_TIMEOUT_NS)) {
            client::fault();
            return -1;
        }
        wait_notification(c.interrupt_fd, std::chrono::milliseconds{ client::RESPONSE_WAIT_MS });
    }
}
extern "C" void protomate_sim_dispatch_interrupt()
{
    auto& c{ simulation::client::connection() };
    c.isr = true;
    const auto flags{ simulation::acquire(c.mapping->memory().irq_flags) };
    if ((flags & 2U) && c.step_callback) {
        c.step_callback();
    }
    if (flags & 4U) {
        simulation::client::dispatch_inputs();
    }
    if (flags & simulation::client::PERIPHERAL_IRQ_MASK) {
        simulation::client::dispatch_timers();
    }
    c.isr = false;
    // Callback execution is bounded and nonblocking. Drain its fixed write batch
    // after callbacks return, while logical time remains frozen by the IRQ handoff.
    for (unsigned i{}; i < c.deferred_count; ++i) {
        if (simulation::client::call(c.deferred[i]).status) {
            simulation::client::fault();
            break;
        }
    }
    c.deferred_count = c.bulk_used = 0;
}
extern "C" void protomate_sim_ack_interrupt()
{
    auto& c{ simulation::client::connection() };
    if (!c.mapping) {
        return;
    }
    auto& m{ c.mapping->memory() };
    simulation::release(m.irq_ack, simulation::acquire(m.irq_sequence));
    simulation::notify(c.acknowledged_fd);
}
