#pragma once
#include "p2p/Bridge.hpp"
#include "p2p/InProcessTransport.hpp"
#include "runtime/thread.hpp"
#include <atomic>
#include <thread>

namespace p2p::test
{
    // Bench-only traffic generator. It has no motor/driver access. Contexts and
    // producer preserve communication priority; the producer stays below the
    // bench main thread so sustained traffic cannot prevent starting the test.
    class CommunicationLoad
    {
        using Clock = std::chrono::steady_clock;
        pnm::msg::Bus ui, io;
        pnm::coro::Context uiContext, ioContext;
        std::array<std::shared_ptr<InProcessTransport>, 2> transport = InProcessTransport::pair();
        Bridge a{ ui, uiContext, transport[0] }, b{ io, ioContext, transport[1] };
        std::optional<pnm::msg::ServiceServer<std::uint32_t, std::uint32_t>> service;
        pnm::msg::ReadinessRegistration readiness;
        std::atomic_bool scheduled{};
        std::jthread uiThread, ioThread, producer;

      public:
        std::atomic_uint32_t completed{}, failures{};
        CommunicationLoad()
        {
            a.importService<std::uint32_t, std::uint32_t>(1, "load.echo");
            b.exportService<std::uint32_t, std::uint32_t>(1, "load.echo");
            b.publishTopic<std::uint32_t>(2, "load.state");
            a.receiveTopic<std::uint32_t>(2, "load.state");
            service.emplace(io.service<std::uint32_t, std::uint32_t>("load.echo").serve([](auto value) {
                return value ^ 0xa5a5a5a5U;
            }));
            readiness = pnm::msg::watch(*service, [this] {
                if (scheduled.exchange(true))
                    return;
                pnm::coro::co_spawn(ioContext, [this](pnm::coro::Context&) -> pnm::coro::Task<void> {
                    scheduled.store(false);
                    service->poll();
                    co_return;
                });
            });
            a.start();
            b.start();
            ioThread = runtime::thread::create_jthread(
              { .name = "load IO", .priority = 3, .stack_size = 16384 }, [this] { ioContext.run(); });
            uiThread = runtime::thread::create_jthread(
              { .name = "load UI", .priority = 4, .stack_size = 16384 }, [this] { uiContext.run(); });
            producer = runtime::thread::create_jthread(
              { .name = "load producer", .priority = 20, .stack_size = 16384 }, [this](std::stop_token stop) {
                while (!stop.stop_requested() &&
                       (a.state() != LinkState::Connected || b.state() != LinkState::Connected))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::uint32_t sequence{};
                while (!stop.stop_requested()) {
                    auto response = ui.service<std::uint32_t, std::uint32_t>("load.echo")
                                      .call(++sequence, std::chrono::milliseconds(200));
                    if (response && *response == (sequence ^ 0xa5a5a5a5U))
                        ++completed;
                    else
                        ++failures;
                    for (unsigned i = 0; i < 8; ++i)
                        io.topic<std::uint32_t>("load.state").publish(sequence * 8 + i);
                }
            });
        }
        ~CommunicationLoad()
        {
            producer.request_stop();
            if (producer.joinable())
                producer.join();
            a.close();
            b.close();
            const auto deadline = Clock::now() + std::chrono::seconds(1);
            while ((a.state() != LinkState::Closed || b.state() != LinkState::Closed) &&
                   Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            readiness.reset();
            uiContext.stop();
            ioContext.stop();
            if (uiThread.joinable())
                uiThread.join();
            if (ioThread.joinable())
                ioThread.join();
        }
    };
}
