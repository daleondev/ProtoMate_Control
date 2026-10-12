#include "common/icc/transport/InProcessTransport.hpp"
#include "io/app/Application.hpp"
#include "ui/app/Application.hpp"
#include "ui/cli/linux/Console.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

int main()
try {
    // Configure routing before any application can log: queued Pneumo records
    // retain their original sink, even if the console replaces it later.
    ui::cli::terminal::configure_logging();

    auto transport = icc::InProcessTransport::pair();
    // Keep application state off the small runtime startup-thread stack.
    auto io = std::make_unique<io::Application>(*transport[0]);
    auto ui = std::make_unique<ui::Application>(*transport[1]);
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
