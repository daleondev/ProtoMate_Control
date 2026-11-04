#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace simulation::assets
{
    // Views refer to immutable static storage embedded by the C++26 compiler.
    // The controller implementation is linked only into the native Application.
    std::span<const std::byte> controllerImage() noexcept;
    std::array<std::span<const std::byte>, 4> robotModels() noexcept;
    std::array<std::span<const std::byte>, 3> collisionLinks() noexcept;
    std::array<std::span<const std::byte>, 3> switchLevers() noexcept;
    struct ShaderSources
    {
        std::string_view vertex{}, fragment{};
    };
    ShaderSources sceneShader() noexcept;
    ShaderSources modelShader() noexcept;
}
