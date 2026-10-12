#include "Services.hpp"
#include "ContextThread.hpp"

#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

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
        pnm::msg::Bus io_bus;
        pnm::msg::Bus ui_bus;
        auto endpoints = icc::InProcessTransport::pair();
        pnm::coro::Context context;
        auto icc_thread = example::run_context(context);

        icc::Bridge io_bridge(io_bus, context, *endpoints[0]);
        icc::Bridge ui_bridge(ui_bus, context, *endpoints[1]);
        io_bridge.registerServices<Services>(ServiceProvider::IO);
        ui_bridge.registerServices<Services>(ServiceProvider::UI);

        // The application owns its handler. The bridge never executes it inline.
        auto provider = io_bus.service<^^Services::demo_io_echo>().serve([](const std::string& request) {
            std::cout << "IO received request: " << request << '\n';
            return "IO says: " + request;
        });
        io_bridge.start();
        ui_bridge.start();

        // Keep the native call handle until completion; destroying it cancels.
        auto call = ui_bus.service<^^Services::demo_io_echo>().request("hello", 2s);
        // In a real application its IO worker dispatches this provider. Here main
        // runs the handler while the dedicated icc thread forwards both directions.
        if (provider.poll(2s) != 1) {
            throw std::runtime_error("No service request received");
        }
        auto response = call.get(); // Wait on main, never on the icc context thread.
        if (!response || *response != "IO says: hello") {
            throw std::runtime_error("Unexpected service result");
        }
        std::cout << "UI received response: " << *response << '\n';

        io_bridge.stop();
        ui_bridge.stop();
        if (io_bridge.failure() || ui_bridge.failure()) {
            throw std::runtime_error("Bridge reported a failure");
        }
        std::cout << "Service round trip verified; bridges stopped cleanly.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "icc service example: " << error.what() << '\n';
        return 1;
    }
}
