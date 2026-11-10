#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace simulation::assets
{
    // Views refer to immutable static storage embedded by the C++26 compiler.
    // The controller implementation is linked only into the native Application.
    std::span<const std::byte> controller_image() noexcept;
    std::array<std::span<const std::byte>, 4> robot_models() noexcept;
    std::array<std::span<const std::byte>, 3> collision_links() noexcept;
    std::array<std::span<const std::byte>, 3> switch_levers() noexcept;
    struct ShaderSources
    {
        std::string_view vertex{}, fragment{};
    };
    ShaderSources scene_shader() noexcept;
    ShaderSources model_shader() noexcept;
}
