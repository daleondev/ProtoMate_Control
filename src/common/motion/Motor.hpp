#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace motion
{
    // ICC values describe motor shafts only: degrees, rpm, degrees/s^2,
    // degrees/s^3 and seconds. No HAL types or mechanical conversions cross ICC.
    enum class MotorId : std::uint8_t { M1, M2, M3, All };
    enum class MotorOperation : std::uint8_t { None, Initialize, Enable, Disable, Stop, Defaults, Speed };
    enum class MotorMotion : std::uint8_t { Relative, Absolute, Home };
    enum class MotorBuffer : std::uint8_t { Aborting, Buffered, BlendingLow, BlendingPrevious, BlendingNext, BlendingHigh };
    enum class MotorOutcome : std::uint8_t { Completed, Stopped, TimedOut, Rejected, Faulted };
    enum class MotorFeedback : std::uint8_t { Encoder, Index };

    inline constexpr std::chrono::milliseconds MOTOR_STATE_PERIOD{ 20 };
    inline constexpr std::chrono::milliseconds MOTOR_STATE_MAX_AGE{ 500 };

    struct MotorState
    {
        double position{}, velocity{}, actual_position{}, actual_velocity{}, feedback_resolution{};
        double acceleration{}, deceleration{}, jerk{};
        bool referenced{}, switch_active{}, position_valid{}, velocity_valid{};
        bool driver_ready{}, fault_active{}, fault_latched{}, warning{};
        MotorFeedback feedback{};
        std::uint8_t fault{};
        std::string driver;
    };

    struct MotorRequest
    {
        MotorOperation operation{ MotorOperation::None };
        MotorId motor{ MotorId::All };
        double velocity{}, acceleration{}, deceleration{}, jerk{};
        // Defaults: bit 0 acceleration, bit 1 deceleration, bit 2 jerk.
        std::uint8_t dynamics_fields{};
    };

    struct MotorSnapshot
    {
        bool enabled{}, ready{};
        std::uint64_t commandGeneration{}, sequence{};
        // IO steady-clock timestamp. UI freshness uses its own receive clock;
        // the cores' steady clocks need not share an epoch.
        std::uint64_t sampled_at_us{};
        std::array<MotorState, 3> motors{};
    };

    struct MotorDefaults
    {
        double acceleration{}, deceleration{}, jerk{};
    };

    struct MotorResponse
    {
        bool ok{ true };
        std::string message;
        // Acknowledgements also carry the generation when a command fails.
        std::uint64_t commandGeneration{}, pulse{};
        MotorDefaults defaults{}; // Only populated for Defaults requests.
    };

    struct MotorGoal
    {
        MotorId motor{ MotorId::M1 };
        MotorMotion motion{ MotorMotion::Relative };
        double position{}, velocity{}, acceleration{}, deceleration{}, jerk{}, timeout{};
        double latch_velocity{ 0.5 };
        MotorBuffer buffer{ MotorBuffer::Aborting };
        // A stop/recovery invalidates submissions made from an earlier snapshot.
        std::uint64_t commandGeneration{};
    };

    struct MotorProgress
    {
        double position{}, velocity{};
        bool referenced{};
    };

    struct MotorResult
    {
        MotorOutcome outcome{ MotorOutcome::Rejected };
        MotorProgress final_state;
    };
}
