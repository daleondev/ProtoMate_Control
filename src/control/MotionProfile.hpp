#pragma once

#include <array>
#include <optional>

namespace motion
{
    // Monotone, one-dimensional profile. SI angular units: rad, rad/s,
    // rad/s^2 and rad/s^3. All phases are analytic constant-jerk polynomials.
    // Storage and evaluation time are bounded independently of pulse count.
    class Profile final
    {
      public:
        struct State
        {
            double position{}, velocity{}, acceleration{};
        };
        struct Limits
        {
            double velocity{}, acceleration{}, deceleration{}, jerk{};
        };
        static std::optional<Profile> create(double distance,
                                             Limits limits,
                                             double start_velocity = 0.0,
                                             double start_acceleration = 0.0,
                                             double end_velocity = 0.0) noexcept;
        State at(double time) const noexcept;
        double timeAt(double position, double hint = 0.0) const noexcept;
        double maximumStepInterval(double step) const noexcept;
        double duration() const noexcept { return m_duration; }
        double distance() const noexcept { return m_distance; }
        double endVelocity() const noexcept { return m_endVelocity; }

      private:
        struct Phase
        {
            double start{}, duration{}, jerk{}, end_position{};
            State state;
        };
        void append(double duration, double jerk, double acceleration) noexcept;
        void changeVelocity(double target, double acceleration, double jerk) noexcept;
        std::array<Phase, 13> m_phases{};
        unsigned m_size{};
        State m_state;
        double m_duration{}, m_distance{}, m_endVelocity{};
    };
}
