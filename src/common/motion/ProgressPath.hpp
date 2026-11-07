#pragma once
#include "Polynomial.hpp"
#include "Profile.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace motion
{
    // Monotone motor displacement as a function of one common path progress.
    // Empty means the existing linear joint path. Curves use local u=[0,1].
    struct ProgressPath
    {
        static constexpr unsigned CAPACITY = 4;
        std::array<Polynomial, CAPACITY> curves{};
        std::array<double, CAPACITY> ends{};
        unsigned size{};
        double at(double progress, unsigned derivative = 0) const noexcept;
        Interval range(unsigned derivative = 0) const noexcept;
        bool valid(double distance) const noexcept;
        std::array<double, 3> dynamicBounds(Profile::Limits progress) const noexcept;
        struct Cursor
        {
            unsigned piece{};
            double progress{};
        };
        // Bounded safeguarded Newton solve. Never evaluates inverse kinematics
        // in the IO worker or an interrupt; the geometric path is already certified.
        double crossing(double position, Cursor& cursor) const noexcept;
    };
}
