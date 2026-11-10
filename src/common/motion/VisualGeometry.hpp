#pragma once
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace motion::visual
{
    // Rigid registration from the evaluated CAD, in metres and radians, Z up.
    // This is visual geometry, not a substitute for machine calibration.
    inline constexpr std::array<double, 3> LENGTHS{ .205, .2234, .135 };
    inline constexpr double SHOULDER_Z{ .052 }, ELBOW_Z{ .3699 }, CARRIAGE_Z{ .3089 };
    inline constexpr double TOOL_Z{ -.280 };
    struct Frame
    {
        std::array<double, 3> translation{};
        double yaw{};
    };
    inline Frame relative(unsigned axis, double joint, const std::array<double, 3>& lengths = LENGTHS)
    {
        if (axis >= 3 || !std::isfinite(joint)) {
            throw std::invalid_argument("invalid visual joint");
        }
        if (axis == 0) {
            return { .translation = { 0, 0, SHOULDER_Z },
                     .yaw = std::remainder(joint, 2 * std::numbers::pi) };
        }
        if (axis == 1) {
            return { .translation = { lengths[0], 0, ELBOW_Z - SHOULDER_Z },
                     .yaw = std::remainder(joint, 2 * std::numbers::pi) };
        }
        return { .translation = { lengths[1] + lengths[2], 0, CARRIAGE_Z - ELBOW_Z + joint }, .yaw = 0 };
    }
}
