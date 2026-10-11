#include "Services.hpp"

#include "p2p/Bridge.hpp"
#include "p2p/InProcessTransport.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main()
{
    using namespace std::chrono_literals;
    using example::ServiceProvider;
    using example::Services;

    try {
        pnm::msg::Bus io_bus, ui_bus;
        auto endpoints = p2p::InProcessTransport::pair();
        pnm::coro::Context context;
        std::jthread p2p_thread([&context](std::stop_token stop) {
            std::stop_callback cancellation{ stop, [&context] { context.stop(); } };
            context.run();
        });

        p2p::Bridge io_bridge(io_bus, context, *endpoints[0]);
        p2p::Bridge ui_bridge(ui_bus, context, *endpoints[1]);
        io_bridge.registerServices<Services>(ServiceProvider::IO);
        ui_bridge.registerServices<Services>(ServiceProvider::UI);

        // The application owns its handler. The bridge never executes it inline.
        auto provider = Services::echo.on(io_bus).serve([](const std::string& request) {
            std::cout << "IO received request: " << request << '\n';
            return "IO says: " + request;
        });
        io_bridge.start();
        ui_bridge.start();

        // Keep the native call handle until completion; destroying it cancels.
        auto call = Services::echo.on(ui_bus).request("hello", 2s);
        // In a real application its IO worker dispatches this provider. Here main
        // runs the handler while the dedicated p2p thread forwards both directions.
        if (provider.poll(2s) != 1)
            throw std::runtime_error("No service request received");
        auto response = call.get(); // Wait on main, never on the p2p context thread.
        if (!response || *response != "IO says: hello")
            throw std::runtime_error("Unexpected service result");
        std::cout << "UI received response: " << *response << '\n';

        io_bridge.stop();
        ui_bridge.stop();
        if (io_bridge.failure() || ui_bridge.failure())
            throw std::runtime_error("Bridge reported a failure");
        std::cout << "Service round trip verified; bridges stopped cleanly.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p2p service example: " << error.what() << '\n';
        return 1;
    }
}
