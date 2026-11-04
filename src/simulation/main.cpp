#include "Session.hpp"
#include "viewer/Viewer.hpp"
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
try {
    simulation::installSignalHandlers();
    bool headless{};
    auto controller{ (std::filesystem::canonical("/proc/self/exe").parent_path() / "Application").string() };
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
        else if (argument == "--controller" && i + 1 < argc)
            controller = argv[++i];
        else if (argument == "--") {
            first_argument = i + 1;
            break;
        }
        else if (argument == "--help") {
            std::puts("protomate-sim [--headless] [--media-dir directory] [--trace file.csv] [--controller "
                      "executable] [-- controller arguments]\nNative hardware simulation; close the robot "
                      "window to continue headless.");
            return 0;
        }
        else
            throw std::invalid_argument{ "unknown simulator argument" };
    }
    simulation::Session session{};
    session.launch(controller.c_str(),
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
    std::fprintf(stderr, "protomate-sim: %s\n", e.what());
    return 1;
}
