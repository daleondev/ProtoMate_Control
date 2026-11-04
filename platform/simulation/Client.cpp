#include "Client.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <pthread.h>
#include <tx_api.h>

namespace simulation::client
{
    namespace
    {
        struct Connection
        {
            std::unique_ptr<Mapping> mapping;
            int request_fd{ -1 }, response_fd{ -1 }, interrupt_fd{ -1 }, acknowledged_fd{ -1 };
            std::uint64_t sequence{};
            std::atomic_bool failed{};
            unsigned depth{};
            bool isr{};
            std::array<hal::GpioInput*, 176> inputs{};
            std::array<void*, 21> timers{};
            std::array<void (*)(void*), 21> timer_callbacks{};
            std::array<Packet, 64> deferred{};
            unsigned deferred_count{};
            std::function<void()> step_callback;
            Connection()
            {
                const auto* value{ std::getenv("PROTOMATE_SIM_FD") };
                if (!value)
                    return;
                try {
                    mapping = std::make_unique<Mapping>(std::stoi(value));
                    request_fd = std::stoi(std::getenv("PROTOMATE_SIM_REQUEST_FD"));
                    response_fd = std::stoi(std::getenv("PROTOMATE_SIM_RESPONSE_FD"));
                    interrupt_fd = std::stoi(std::getenv("PROTOMATE_SIM_INTERRUPT_FD"));
                    acknowledged_fd = std::stoi(std::getenv("PROTOMATE_SIM_ACK_FD"));
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
            if (!c.failed.exchange(true))
                std::fputs("simulation: hardware connection lost; execution counts are unknown\n", stderr);
            if (c.mapping)
                release(c.mapping->memory().closed, 1U);
        }
    }
    bool connected() noexcept { return connection().mapping != nullptr; }
    bool failed() noexcept { return connection().failed.load(); }
    SharedMemory* memory() noexcept { return connected() ? &connection().mapping->memory() : nullptr; }
    bool inInterrupt() noexcept { return connection().isr; }
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
        if (!c.mapping || c.failed)
            return result;
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
        release(m.client_heartbeat, hostNow());
        request.sequence = ++c.sequence;
        request.session = m.session;
        try {
            auto frame{ encode(request) };
            const auto deadline{ hostNow() + 1'000'000'000ULL };
            while (enqueue(m.registers[0], frame) == QueueResult::Full) {
                if (c.isr || hostNow() >= deadline) {
                    fault();
                    return result;
                }
                waitNotification(c.response_fd, 1);
            }
            notify(c.request_fd);
            for (;;) {
                std::array<std::byte, FRAME_BYTES> bytes{};
                std::size_t size{};
                const auto state{ dequeue(m.registers[1], bytes, size) };
                if (state == QueueResult::Success) {
                    auto reply{ decode(std::span{ bytes }.first(size)) };
                    if (reply.session != m.session) {
                        fault();
                        return result;
                    }
                    if (reply.sequence == request.sequence)
                        return reply;
                    if (reply.sequence > request.sequence) {
                        fault();
                        return result;
                    }
                }
                else if (state != QueueResult::Empty) {
                    fault();
                    return result;
                }
                if (acquire(m.closed) || hostNow() >= deadline) {
                    fault();
                    return result;
                }
                waitNotification(c.response_fd, 10);
            }
        } catch (...) {
            fault();
            return result;
        }
    }
    Packet command(Operation operation, std::array<std::uint64_t, 8> arguments) noexcept
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
        if (!inInterrupt())
            command(Operation::Sample);
        result.tick = acquire(m->step_sample[0]);
        result.running = acquire(m->step_sample[1]);
        result.error = acquire(m->step_sample[2]) || failed();
        for (unsigned i{}; i < 3; ++i) {
            const auto o{ 3 + i * 8 };
            auto& c{ result.channels[i] };
            c.tick = acquire(m->step_sample[o]);
            c.compare = acquire(m->step_sample[o + 1]);
            c.remaining = acquire(m->step_sample[o + 2]);
            c.target = static_cast<std::uint8_t>(acquire(m->step_sample[o + 3]));
            c.transfer_complete = acquire(m->step_sample[o + 4]);
            c.compare_pending = acquire(m->step_sample[o + 5]);
            c.high = acquire(m->step_sample[o + 6]);
        }
        return result;
    }
    void setStepCallback(std::function<void()> callback) { connection().step_callback = std::move(callback); }
    void registerTimer(unsigned index, void* object, void (*callback)(void*)) noexcept
    {
        connection().timers.at(index) = object;
        connection().timer_callbacks.at(index) = callback;
    }
    void dispatchTimers() noexcept
    {
        auto& c{ connection() };
        for (unsigned i{}; i < c.timers.size(); ++i)
            if (c.timers[i])
                c.timer_callbacks[i](c.timers[i]);
    }
    void registerInput(unsigned pin, hal::GpioInput* input) noexcept { connection().inputs.at(pin) = input; }
    void dispatchInputs() noexcept
    {
        auto* m{ memory() };
        if (!m)
            return;
        for (unsigned index{}; index < m->gpio.size(); ++index) {
            auto* input{ connection().inputs[index] };
            if (input)
                input->setSimulatedLevel(acquire(m->gpio[index]) ? hal::gpio::Level::High
                                                                 : hal::gpio::Level::Low);
        }
    }
    void enter() noexcept
    {
        auto& c{ connection() };
        if (c.mapping && c.depth++ == 0 && !c.isr)
            command(Operation::Begin);
    }
    void leave() noexcept
    {
        auto& c{ connection() };
        if (c.mapping && c.depth && --c.depth == 0 && !c.isr)
            command(Operation::End);
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
extern "C" int protomate_sim_wait_interrupt()
{
    using namespace simulation;
    auto& c{ client::connection() };
    if (!c.mapping)
        return -1;
    auto& m{ c.mapping->memory() };
    for (;;) {
        release(m.client_heartbeat, hostNow());
        if (acquire(m.closed) || c.failed)
            return -1;
        if (acquire(m.irq_sequence) != acquire(m.irq_ack))
            return static_cast<int>(acquire(m.irq_flags));
        const auto last{ acquire(m.server_heartbeat) };
        if (hostNow() - last > 1'000'000'000ULL) {
            client::fault();
            return -1;
        }
        waitNotification(c.interrupt_fd, 10);
    }
}
extern "C" void protomate_sim_dispatch_interrupt()
{
    auto& c{ simulation::client::connection() };
    c.isr = true;
    const auto flags{ simulation::acquire(c.mapping->memory().irq_flags) };
    if ((flags & 2U) && c.step_callback)
        c.step_callback();
    if (flags & 4U)
        simulation::client::dispatchInputs();
    if (flags & 24U)
        simulation::client::dispatchTimers();
    c.isr = false;
    // Callback execution is bounded and nonblocking. Drain its fixed write batch
    // after callbacks return, while logical time remains frozen by the IRQ handoff.
    for (unsigned i{}; i < c.deferred_count; ++i) {
        if (simulation::client::call(c.deferred[i]).status) {
            simulation::client::fault();
            break;
        }
    }
    c.deferred_count = 0;
}
extern "C" void protomate_sim_ack_interrupt()
{
    auto& c{ simulation::client::connection() };
    if (!c.mapping)
        return;
    auto& m{ c.mapping->memory() };
    simulation::release(m.irq_ack, simulation::acquire(m.irq_sequence));
    simulation::notify(c.acknowledged_fd);
}
