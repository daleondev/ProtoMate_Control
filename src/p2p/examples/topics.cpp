#include "Topics.hpp"

#include "p2p/Bridge.hpp"
#include "p2p/InProcessTransport.hpp"

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
    using example::Direction;
    using example::Topics;

    try {
        pnm::msg::Bus io_bus, ui_bus;
        auto endpoints = p2p::InProcessTransport::pair();
        pnm::coro::Context context;

        // One dedicated p2p thread runs both bridges, sleeping when idle.
        // Declare it before the bridges: their destructors drain their loops
        // before this thread requests context shutdown and joins, even on error.
        std::jthread p2p_thread([&context](std::stop_token stop) {
            std::stop_callback cancellation{ stop, [&context] { context.stop(); } };
            context.run();
        });

        p2p::Bridge io_bridge(io_bus, context, *endpoints[0]);
        p2p::Bridge ui_bridge(ui_bus, context, *endpoints[1]);
        io_bridge.registerTopics<Topics>(Direction::IOToUI);
        ui_bridge.registerTopics<Topics>(Direction::UIToIO);

        // These are ordinary local subscriptions. The bridges handle transport.
        std::string received_state, received_status;
        auto ui_subscription = Topics::state.on(ui_bus).subscribe([&](const std::string& value) {
            received_state = value;
            std::cout << "UI received demo/io/state: " << value << '\n';
        });
        auto io_subscription = Topics::status.on(io_bus).subscribe([&](const std::string& value) {
            received_status = value;
            std::cout << "IO received demo/ui/status: " << value << '\n';
        });

        io_bridge.start();
        ui_bridge.start();
        Topics::state.on(io_bus).publish("idle");
        Topics::status.on(ui_bus).publish("ready");

        // Only the example's main thread waits here; both bridge directions
        // continue concurrently on the p2p thread. poll(timeout) sleeps, not spins.
        if (ui_subscription.poll(2s) != 1 || io_subscription.poll(2s) != 1)
            throw std::runtime_error("Timed out waiting for a forwarded topic");
        if (received_state != "idle" || received_status != "ready")
            throw std::runtime_error("Forwarded topic value did not match");

        io_bridge.stop();
        ui_bridge.stop();
        if (io_bridge.failure() || ui_bridge.failure())
            throw std::runtime_error("Bridge reported a failure");
        std::cout << "Both directions verified; bridges stopped cleanly.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p2p example: " << error.what() << '\n';
        return 1;
    }
}
