#pragma once
#include "icc/bridge/Bridge.hpp"
#include "icc/transport/InProcessTransport.hpp"
#include "runtime/thread.hpp"
#include <atomic>
#include <thread>

namespace icc::test
{
    enum class ServiceProvider
    {
        IO,
        UI
    };
    enum class TopicPublisher
    {
        IO,
        UI
    };
    struct Services
    {
        [[= ServiceProvider::IO]] pnm::msg::Service<std::uint32_t, std::uint32_t> load_echo;
    };
    struct Topics
    {
        [[= TopicPublisher::IO]] pnm::msg::Topic<std::uint32_t> load_state;
    };

    // Bench-only traffic generator. It has no motor/driver access. Contexts and
    // producer preserve communication priority; the producer stays below the
    // bench main thread so sustained traffic cannot prevent starting the test.
    class CommunicationLoad
    {
        pnm::msg::Bus ui, io;
        pnm::coro::Context uiContext, ioContext;
        std::array<std::shared_ptr<InProcessTransport>, 2> transport = InProcessTransport::pair();
        Bridge a{ ui, uiContext, *transport[0] }, b{ io, ioContext, *transport[1] };
        std::optional<pnm::msg::ServiceServer<std::uint32_t, std::uint32_t>> service;
        pnm::msg::ReadinessRegistration readiness;
        std::atomic_bool scheduled{};
        std::jthread uiThread, ioThread, producer;

      public:
        std::atomic_uint32_t completed{}, failures{};
        CommunicationLoad()
        {
            a.registerServices<Services>(ServiceProvider::UI);
            b.registerServices<Services>(ServiceProvider::IO);
            a.registerTopics<Topics>(TopicPublisher::UI);
            b.registerTopics<Topics>(TopicPublisher::IO);
            service.emplace(
              io.service<^^Services::load_echo>().serve([](auto value) { return value ^ 0xa5a5a5a5U; }));
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
                while (!stop.stop_requested() && (a.connectionStatus().state != ConnectionState::Connected ||
                                                  b.connectionStatus().state != ConnectionState::Connected))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::uint32_t sequence{};
                while (!stop.stop_requested()) {
                    auto response =
                      ui.service<^^Services::load_echo>().call(++sequence, std::chrono::milliseconds(200));
                    if (response && *response == (sequence ^ 0xa5a5a5a5U))
                        ++completed;
                    else
                        ++failures;
                    for (unsigned i = 0; i < 8; ++i)
                        io.topic<^^Topics::load_state>().publish(sequence * 8 + i);
                }
            });
        }
        ~CommunicationLoad()
        {
            producer.request_stop();
            if (producer.joinable())
                producer.join();
            a.stop();
            b.stop();
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
