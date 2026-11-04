#include "common/icc/transport/InProcessTransport.hpp"
#include "io/app/Application.hpp"
#include "simulation/Client.hpp"
#include "simulation/ControllerLink.hpp"
#include "ui/app/Application.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

int main()
try {
    if (!simulation::client::connected() || simulation::client::failed()) {
        std::fputs("Start through protomate-sim to connect simulated hardware.\n", stderr);
        return EXIT_FAILURE;
    }
    simulation::ControllerLink simulator{};
    auto transport{ icc::InProcessTransport::pair() };
    // Keep application state off the small runtime startup-thread stack.
    auto io{ std::make_unique<io::Application>(*transport[0]) };
    auto ui{ std::make_unique<ui::Application>(*transport[1]) };
    io->start();
    ui->start();

    // Both applications are running before we wait. Their lifecycle workers
    // handle stop requests and peer loss without relying on this thread.
    io->join();
    ui->join();
    return io->connectionStatus().state == icc::ConnectionState::Failed ||
               ui->connectionStatus().state == icc::ConnectionState::Failed
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::fputs("Application startup failed: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}
