#include "ExecutableImage.hpp"
#include "Session.hpp"
#include "viewer/Viewer.hpp"
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>

extern "C" {
extern const std::byte protomate_controller_image_start[];
extern const std::uint64_t protomate_controller_image_size;
}

int main(int argc, char** argv)
try {
    simulation::installSignalHandlers();
    bool headless{};
    int first_argument{ argc };
    for (int i{ 1 }; i < argc; ++i) {
        const std::string_view argument{ argv[i] };
        if (argument == "--headless")
            headless = true;
        else if (argument == "--trace" && i + 1 < argc) {
            setenv("PROTOMATE_TRACE", argv[++i], 1);
        }
        else if (argument == "--media-dir" && i + 1 < argc) {
            setenv("PROTOMATE_MEDIA_DIR", argv[++i], 1);
        }
        else if (argument == "--") {
            first_argument = i + 1;
            break;
        }
        else if (argument == "--help") {
            std::puts(
              "Application [--headless] [--media-dir directory] [--trace file.csv] [-- controller "
              "arguments]\n"
              "One executable, two processes: native simulation and the embedded ThreadX controller.\n"
              "Close the robot window to continue headless.");
            return 0;
        }
        else
            throw std::invalid_argument{ "unknown simulator argument" };
    }
    simulation::ExecutableImage controller{ { protomate_controller_image_start,
                                              protomate_controller_image_size } };
    simulation::Session session{};
    session.launch(controller.fd(),
                   std::span{ argv + first_argument, static_cast<std::size_t>(argc - first_argument) });
    session.start();
    if (!headless) {
        try {
            simulation::viewer::run(session);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "simulation viewer unavailable: %s; continuing headless\n", e.what());
        }
    }
    while (session.running())
        std::this_thread::sleep_for(std::chrono::milliseconds{ 20 });
    return session.childStatus();
} catch (const std::exception& e) {
    std::fprintf(stderr, "Application: %s\n", e.what());
    return 1;
}
