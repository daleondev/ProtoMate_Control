#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace motion
{
    // Monotone, one-dimensional profile in consistent scalar units (radians for
    // motors, normalized path progress for coordinated moves), with seconds as
    // time. All phases are analytic constant-jerk polynomials.
    // Storage and evaluation time are bounded independently of pulse count.
    class Profile final
    {
      public:
        static constexpr unsigned MAX_PHASES = 13;
        struct State
        {
            double position{}, velocity{}, acceleration{};
        };
        struct Limits
        {
            double velocity{}, acceleration{}, deceleration{}, jerk{};
        };
        struct PhaseDescription
        {
            double duration{}, jerk{}, position{}, velocity{}, acceleration{};
        };
        struct Description
        {
            std::array<PhaseDescription, MAX_PHASES> phases{};
            unsigned size{};
            double distance{}, end_velocity{};
        };
        Description description() const noexcept;
        static std::optional<Profile> fromDescription(const Description& value) noexcept;

        static std::optional<Profile> create(double distance,
                                             Limits limits,
                                             double start_velocity = 0.0,
                                             double start_acceleration = 0.0,
                                             double end_velocity = 0.0) noexcept;
        State at(double time) const noexcept;
        struct TimeHint
        {
            double seconds;
        };
        double timeAt(double position, TimeHint hint = { .seconds = 0.0 }) const noexcept;
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
        struct PhaseChange
        {
            double duration, jerk, acceleration;
        };
        struct VelocityChange
        {
            double target, acceleration, jerk;
        };
        enum class Preparation : std::uint8_t
        {
            Invalid,
            Complete,
            Continue
        };
        void append(PhaseChange change) noexcept;
        void changeVelocity(VelocityChange change) noexcept;
        bool normalizeAcceleration(Limits limits) noexcept;
        Preparation normalizeVelocity(Limits limits) noexcept;
        bool appendCruise(Limits limits) noexcept;
        bool validEndpoint() const noexcept;
        static double phaseTimeAt(const Phase& p, double position, TimeHint hint) noexcept;
        std::array<Phase, MAX_PHASES> m_phases{};
        unsigned m_size{};
        State m_state;
        double m_duration{}, m_distance{}, m_endVelocity{};
    };
}
