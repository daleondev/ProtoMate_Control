#pragma once

#include "hal/core/core.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InterCoreTransport.hpp"
#include "runtime/thread.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace bench
{
    enum class Publisher : std::uint8_t
    {
        IO,
        UI
    };

    struct Command
    {
        std::uint32_t sequence{};
        std::uint32_t test{};
    };
    struct Report
    {
        std::uint32_t sequence{}, test{}, passed{}, check{};
        std::array<std::uint64_t, 3> pulses{};
        std::array<std::uint32_t, 3> completions{};
        std::uint32_t progress{}, guard_stopped{};
        std::uint64_t changed_at{};
        std::uint32_t load_received{}, load_errors{};
        std::uint32_t service_cycles{}, stop_cycles{};
    };
    struct Load
    {
        std::uint32_t sequence{};
        std::array<std::uint32_t, 63> words{};
        bool operator==(const Load&) const = default;
    };
    inline Load packet(std::uint32_t sequence)
    {
        Load value{ .sequence = sequence };
        for (std::size_t i{}; i < value.words.size(); ++i) {
            value.words[i] = sequence ^ (static_cast<std::uint32_t>(i) * 0x9E3779B9U);
        }
        return value;
    }
    struct Topics
    {
        [[= Publisher::UI]] pnm::msg::Topic<Command> bench_command;
        [[= Publisher::IO]] pnm::msg::Topic<Report> bench_report;
        [[= Publisher::UI]] pnm::msg::Topic<Load> bench_load;
        [[= Publisher::IO]] pnm::msg::Topic<Load> bench_echo;
    };

    // Independent test endpoints using the production transport/runtime. No
    // test routes or workers are added to the normal application catalogs.
    class Link final
    {
      public:
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        icc::InterCoreTransport transport{ hal::core::create_intercore_channel() };
        std::jthread worker;
        icc::Bridge bridge{ bus, context, transport };

        explicit Link(Publisher publisher)
        {
            bridge.registerTopics<Topics>(publisher);
            worker = runtime::thread::create_jthread({ .name = "bench.icc",
                                                       .priority = publisher == Publisher::IO ? 2 : 4,
                                                       .stack_size = 32U * 1024U },
                                                     [this](std::stop_token stop) {
                std::stop_callback cancel{ stop, [this] { context.stop(); } };
                context.run();
            });
            bridge.start();
        }
        ~Link()
        {
            bridge.stop();
            context.stop();
            if (worker.joinable()) {
                worker.join();
            }
        }
        Link(const Link&) = delete;
        Link& operator=(const Link&) = delete;
        bool connected() const { return bridge.connectionStatus().state == icc::ConnectionState::Connected; }
    };
}
