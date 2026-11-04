#pragma once
#include "ModelAsset.hpp"
namespace simulation
{
    class Session;
}
namespace simulation::viewer
{
    struct ViewerOptions
    {
        std::filesystem::path model_directory{};
        ModelCoordinates coordinates{};
    };
    void run(Session& session, const ViewerOptions& options = {});
}
