#pragma once
#include "Bench.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InterCoreTransport.hpp"
#include <string>

namespace motion_bench
{
    enum class Peer
    {
        IO,
        UI
    };
    struct Services
    {
        [[= Peer::IO]] pnm::msg::Service<std::string, std::string> echo;
        [[ = Peer::IO, = icc::Lane::Urgent ]] pnm::msg::Service<std::string, std::string> urgent;
    };
    struct Topics
    {
        [[= Peer::UI]] pnm::msg::Topic<std::string> data;
        [[= Peer::IO]] pnm::msg::Topic<std::string> reply;
        [[ = Peer::UI,
           = icc::Lane::Background,
           = pnm::msg::SubscriptionOptions{ .capacity = 8 } ]] pnm::msg::Topic<std::string>
          log;
    };
    struct Actions
    {
        [[= Peer::IO]] pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> action;
    };
    class Link
    {
      public:
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        icc::InterCoreTransport transport;
        icc::Bridge bridge;
        std::jthread worker;
        bool started{};
        bool protectedScheduling{ protectedAdmission };
        explicit Link(std::shared_ptr<hal::IInterCoreChannel> channel)
          : transport(std::move(channel))
          , bridge(bus, context, transport)
        {
            bridge.registerServices<Services>(SIDE == 0 ? Peer::IO : Peer::UI);
            bridge.registerTopics<Topics>(SIDE == 0 ? Peer::IO : Peer::UI);
            bridge.registerActions<Actions>(SIDE == 0 ? Peer::IO : Peer::UI);
            worker = runtime::thread::create_jthread(
              { .name = "bench.icc", .priority = SIDE == 0 ? (protectedScheduling ? 5 : 4) : 4, .stack_size = 32U * 1024 },
              [this](std::stop_token stop) {
                  if (dispatchMode != 2) {
                      context.run();
                      return;
                  }
                  // Bounded scheduling experiment. The producer is below motion
                  // admission; periodic suspension lets background workers run.
                  while (!stop.stop_requested()) {
                      context.poll(8);
                      std::this_thread::sleep_for(1ms);
                  }
              });
        }
        ~Link()
        {
            bridge.stop();
            context.stop();
            worker.request_stop();
            worker.join();
        }
        void start()
        {
            if (!started) {
                bridge.start();
                started = true;
            }
            auto end = std::chrono::steady_clock::now() + 3s;
            while (bridge.connectionStatus().state != icc::ConnectionState::Connected) {
                if (std::chrono::steady_clock::now() > end)
                    throw std::runtime_error("bench ICC handshake timeout");
                std::this_thread::sleep_for(1ms);
            }
        }
    };
    Link& link(std::shared_ptr<hal::IInterCoreChannel> channel);
}
