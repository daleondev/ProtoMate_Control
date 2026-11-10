#include "ExecutableImage.hpp"
#include "Session.hpp"
#include "assets/EmbeddedAssets.hpp"
#include "viewer/Viewer.hpp"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>

namespace
{
    constexpr auto POLL_PERIOD{ std::chrono::milliseconds{ 20 } };
    double number(std::string_view input)
    {
        double value{};
        const auto result{ std::from_chars(input.data(), input.data() + input.size(), value) };
        if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || !std::isfinite(value)) {
            throw std::invalid_argument{ "invalid numeric model option" };
        }
        return value;
    }
    simulation::viewer::UpAxis up_axis(std::string_view value)
    {
        if (value == "y") {
            return simulation::viewer::UpAxis::Y;
        }
        if (value == "z") {
            return simulation::viewer::UpAxis::Z;
        }
        throw std::invalid_argument{ "--model-up expects y or z" };
    }
}

int main(int argc, char** argv)
try {
    simulation::install_signal_handlers();
    bool headless{};
    simulation::SessionOptions options{};
    simulation::viewer::ViewerOptions viewer_options{};
    int first_argument{ argc };
    for (int i{ 1 }; i < argc; ++i) {
        const std::string_view argument{ argv[i] };
        if (argument == "--headless") {
            headless = true;
        }
        else if (argument == "--trace" && i + 1 < argc) {
            options.trace_path = argv[++i];
        }
        else if (argument == "--media-dir" && i + 1 < argc) {
            options.media_directory = argv[++i];
        }
        else if (argument == "--robot-models" && i + 1 < argc) {
            viewer_options.model_directory = argv[++i];
        }
        else if (argument == "--model-scale" && i + 1 < argc) {
            viewer_options.coordinates.millimetres_per_unit = number(argv[++i]);
        }
        else if (argument == "--model-up" && i + 1 < argc) {
            viewer_options.coordinates.up = up_axis(argv[++i]);
        }
        else if (argument == "--") {
            first_argument = i + 1;
            break;
        }
        else if (argument == "--help") {
            std::puts(
              "Application [--headless] [--media-dir directory] [--trace file.csv] [-- controller "
              "arguments]\n"
              "Models: --robot-models DIR [--model-scale mm-per-unit] [--model-up y|z]\n"
              "        defaults: 1000, y (coordinates after Assimp import).\n"
              "DIR contains base_link/base_link.dae and link_1..3/link_1..3.dae; defaults are embedded "
              "Fusion exports.\n"
              "One executable, two processes: native simulation and the embedded ThreadX controller.\n"
              "Close the robot window to continue headless.");
            return 0;
        }
        else {
            throw std::invalid_argument{ "unknown simulator argument" };
        }
    }
    viewer_options.coordinates.matrix();
    simulation::viewer::robot_model_transforms(options.profile, { 0, 0, 0 });
    simulation::ExecutableImage controller{ simulation::assets::controller_image() };
    simulation::Session session{ std::move(options) };
    session.launch(controller.fd(),
                   std::span{ argv + first_argument, static_cast<std::size_t>(argc - first_argument) });
    session.start();
    if (!headless) {
        try {
            simulation::viewer::run(session, viewer_options);
        } catch (const std::exception& e) {
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
            std::fprintf(stderr, "simulation viewer unavailable: %s; continuing headless\n", e.what());
        }
    }
    while (session.running()) {
        std::this_thread::sleep_for(POLL_PERIOD);
    }
    return session.childStatus();
} catch (const std::exception& e) {
    // Keep failure diagnostics nonthrowing, including allocation-failure paths.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
    std::fprintf(stderr, "Application: %s\n", e.what());
    return 1;
}
