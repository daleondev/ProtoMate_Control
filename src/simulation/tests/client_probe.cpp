#include "simulation/Client.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    using namespace simulation;
    const std::string_view mode{ argc > 1 ? argv[1] : "" };
    if (mode == "invalid-session")
        return !client::connected() && client::failed() ? 0 : 1;
    if (!client::connected() || client::failed())
        return 2;
    if (mode == "scheduling") {
        // A slow runnable controller must finish handling the current virtual
        // instant before more timer interrupts wake higher-priority work.
        std::this_thread::sleep_for(std::chrono::milliseconds{ 2 });
        const auto simulated{ client::now() };
        const auto wall{ hostNow() };
        while (hostNow() - wall < 50'000'000ULL) {
            if (client::now() != simulated)
                return 10;
        }
        // Register requests remain serviced while logical time is held.
        if (client::command(Operation::Hello).status || client::now() != simulated)
            return 11;
        std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        if (client::now() < simulated + 10'000'000ULL)
            return 12;
        std::puts("PASS: simulation client regression");
        return 0;
    }
    client::Transaction transaction{};
    if (mode == "deferred") {
        client::setStepCallback([] {
            std::array<std::byte, sizeof(hal::util::StepBuffer)> bytes{};
            for (unsigned i{}; i < 4; ++i) {
                put(bytes, 0, i + 100, 4);
                if (!client::publishBank(0, 0, bytes))
                    std::terminate();
            }
        });
        release(client::memory()->irq_flags, 2U);
        protomate_sim_dispatch_interrupt();
        client::setStepCallback({});
        if (client::failed() || client::command(Operation::Hello).status)
            return 3;
    }
    else if (mode == "corrupt") {
        auto& queue{ client::memory()->registers[0] };
        release(queue.written, acquire(queue.read) + static_cast<std::uint32_t>(QUEUE_CAPACITY) + 1U);
        const auto before{ hostNow() };
        if (!client::command(Operation::Hello).status || !client::failed())
            return 4;
        if (hostNow() - before > 500'000'000ULL)
            return 5;
    }
    else if (mode == "reply") {
        auto& shared{ *client::memory() };
        const auto frame{ encode(
          { .operation = Operation::Power, .sequence = 999, .session = shared.session }) };
        if (enqueue(shared.registers[1], frame) != QueueResult::Success)
            return 6;
        if (!client::command(Operation::Hello).status || !client::failed())
            return 7;
    }
    else if (mode == "startup") {
        if (client::command(Operation::Hello).status)
            return 8;
    }
    else
        return 9;
    std::puts("PASS: simulation client regression");
    return 0;
}
