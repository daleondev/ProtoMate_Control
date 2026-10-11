#pragma once
#include "AxisConversion.hpp"
#include "Completion.hpp"
#include "MotionProfile.hpp"
#include "MotorTypes.hpp"
#include "hal/board/board.hpp"
#include "hal/devices/itf/IConfigurableStepperDriver.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
using namespace pnm::units::literals;
namespace control
{
    struct ControllerTypes
    {
        using MotorId = hal::board::MotorId;
        using MotionId = std::uint64_t;
        struct AxisConfig
        {
            pnm::units::Angle reference_position;
            pnm::units::Angle full_step_angle;
            std::size_t microsteps{};
        };
        template<typename Position, typename Speed, typename Acceleration, typename Jerk>
        struct MoveRequest
        {
            Position position;
            Speed velocity;
            Acceleration acceleration{};
            Acceleration deceleration{};
            Jerk jerk{};
            BufferMode buffer{ BufferMode::Aborting };
            pnm::units::Time timeout{ 0_s };
            bool absolute{};
        };
        using Move = MoveRequest<pnm::units::Angle,
                                 pnm::units::AngularVelocity,
                                 pnm::units::AngularAcceleration,
                                 pnm::units::AngularJerk>;
        using LinearMove =
          MoveRequest<pnm::units::Distance, pnm::units::Velocity, pnm::units::Acceleration, pnm::units::Jerk>;
        using Conversion = std::variant<RotaryAxisConversion, LinearAxisConversion>;
        using AxisMove = std::variant<Move, LinearMove>;
        using AxisSpeed = std::variant<pnm::units::AngularVelocity, pnm::units::Velocity>;
        struct LinearDefaults
        {
            pnm::units::Acceleration acceleration;
            pnm::units::Acceleration deceleration;
            pnm::units::Jerk jerk;
        };
        using AxisDefaults = std::variant<MotionDefaults, LinearDefaults>;
        struct Motion
        {
            MotionId id;
            MotorId motor{};
            // Empty means outstanding (planning, executing, or queued).
            std::optional<MotorResult> result;
            // Retained by the submitter even after the bounded results expire.
            // A Robot can await completion without polling this controller.
            std::shared_future<MotorResult> completion;
            Completion<MotorResult> observed;
            // False after a transport failure: a local stop was requested, but
            // the UI has no terminal acknowledgement from the execution owner.
            bool terminal_confirmed{ true };
        };
        struct AxisStatus
        {
            MotorId motor{};
            pnm::units::Angle position;
            pnm::units::AngularVelocity velocity;
            pnm::Result<pnm::units::Angle> actual_position;
            pnm::Result<pnm::units::AngularVelocity> actual_velocity;
            bool referenced;
            bool reference_switch_active;
            std::size_t outstanding;
            // Configuration and motor state from the same locked snapshot.
            // Empty until the application's mechanics have been supplied.
            std::optional<Conversion> conversion;
            hal::device::IMotorFeedback::Source feedback_source;
            pnm::units::Angle feedback_resolution;
        };
        struct Status
        {
            bool enabled{};
            hal::step::Status generator;
            std::array<AxisStatus, 3> axes;
            bool coordinated{};
            std::uint64_t configuration_revision{}, command_revision{};
        };

        using DriverConfiguration = hal::device::IConfigurableStepperDriver::Configuration;
        struct DriverStatus
        {
            MotorId motor{};
            std::string name;
            hal::device::IStepperDriver::Status status;
            std::optional<hal::device::IConfigurableStepperDriver::Snapshot> configuration;
        };
        enum class Operation : std::uint8_t
        {
            Move,
            Reference
        };
        struct GroupMotion
        {
            MotionId id;
            Operation operation;
            pnm::units::Time duration;
            std::optional<MotorResult> result;
            std::shared_future<MotorResult> completion;
            Completion<MotorResult> observed;
            // False after a transport failure: a local stop was requested, but
            // the UI has no terminal acknowledgement from the execution owner.
            bool terminal_confirmed{ true };
        };
        struct PlanningSnapshot
        {
            Status status;
            std::array<MotionDefaults, 3> defaults;
            std::array<AxisConfig, 3> configuration;
        };
        struct Trajectory
        {
            std::uint64_t configuration_revision{}, command_revision{};
            std::array<pnm::units::Angle, 3> origin{};
            motion::Profile::Description profile;
            std::array<std::uint64_t, 3> pulses{};
            std::array<bool, 3> forward{};
            pnm::units::Time duration;
        };
    };
}
