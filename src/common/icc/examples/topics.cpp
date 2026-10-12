#include "Topics.hpp"
#include "ContextThread.hpp"

#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

int main()
{
    using namespace std::chrono_literals;
    using example::TopicPublisher;
    using example::Topics;

    try {
        pnm::msg::Bus io_bus;
        pnm::msg::Bus ui_bus;
        auto endpoints = icc::InProcessTransport::pair();
        pnm::coro::Context context;

        // One dedicated icc thread runs both bridges, sleeping when idle.
        // Declare it before the bridges: their destructors drain their loops
        // before this thread requests context shutdown and joins, even on error.
        auto icc_thread = example::run_context(context);

        icc::Bridge io_bridge(io_bus, context, *endpoints[0]);
        icc::Bridge ui_bridge(ui_bus, context, *endpoints[1]);
        io_bridge.registerTopics<Topics>(TopicPublisher::IO);
        ui_bridge.registerTopics<Topics>(TopicPublisher::UI);

        // These are ordinary local subscriptions. The bridges handle transport.
        std::string received_state;
        std::string received_status;
        auto ui_subscription =
          ui_bus.topic<^^Topics::demo_io_state>().subscribe([&](const std::string& value) {
            received_state = value;
            std::cout << "UI received demo/io/state: " << value << '\n';
        });
        auto io_subscription = io_bus.topic<^^Topics::status>().subscribe([&](const std::string& value) {
            received_status = value;
            std::cout << "IO received demo/ui/status: " << value << '\n';
        });

        io_bridge.start();
        ui_bridge.start();
        io_bus.topic<^^Topics::demo_io_state>().publish("idle");
        ui_bus.topic<^^Topics::status>().publish("ready");

        // Only the example's main thread waits here; both bridge directions
        // continue concurrently on the icc thread. poll(timeout) sleeps, not spins.
        if (ui_subscription.poll(2s) != 1 || io_subscription.poll(2s) != 1) {
            throw std::runtime_error("Timed out waiting for a forwarded topic");
        }
        if (received_state != "idle" || received_status != "ready") {
            throw std::runtime_error("Forwarded topic value did not match");
        }

        io_bridge.stop();
        ui_bridge.stop();
        if (io_bridge.failure() || ui_bridge.failure()) {
            throw std::runtime_error("Bridge reported a failure");
        }
        std::cout << "Both directions verified; bridges stopped cleanly.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "icc example: " << error.what() << '\n';
        return 1;
    }
}
