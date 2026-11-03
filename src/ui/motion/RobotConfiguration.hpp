#pragma once

#include "AxisConversion.hpp"
#include "ScaraKinematics.hpp"
#include "common/motion/Trajectory.hpp"
#include <array>
#include <optional>

namespace ui::motion
{
    using JointPosition = ScaraKinematics::JointPosition;
    using CartesianPosition = ScaraKinematics::CartesianPosition;
    using RobotError = ::motion::trajectory::Error;

    struct RotaryDynamics
    {
        pnm::units::AngularVelocity velocity{};
        pnm::units::AngularAcceleration acceleration{}, deceleration{};
        pnm::units::AngularJerk jerk{};
    };
    struct LinearDynamics
    {
        pnm::units::Velocity velocity{};
        pnm::units::Acceleration acceleration{}, deceleration{};
        pnm::units::Jerk jerk{};
    };
    struct HomeConfiguration
    {
        bool configured{};
        bool positive{};
        pnm::units::AngularVelocity seek{}, latch{};
        pnm::units::Angle backoff{}, maximum_travel{};
        pnm::units::Time timeout{};
    };
    struct WorkFrame
    {
        CartesianPosition origin{};
        pnm::units::Angle yaw{};
    };
    struct RobotConfiguration
    {
        enum class Purpose { Simulation, Physical };
        Purpose purpose{Purpose::Simulation};
        bool calibrated{};
        std::uint64_t expected_hardware{};
        std::array<std::uint8_t,3> motor_for_joint{0,1,2};
        std::array<RotaryAxisConversion::Config,2> rotary{};
        LinearAxisConversion::Config linear{};
        ScaraKinematics::Config geometry{};
        std::array<RotaryDynamics,2> rotary_dynamics{};
        LinearDynamics linear_dynamics{};
        CartesianPosition workspace_minimum{}, workspace_maximum{};
        WorkFrame work{};
        pnm::units::Velocity feed{};
        pnm::units::Distance path_tolerance{}, blend_tolerance{};
        JointPosition joint_blend_tolerance{};
        std::array<std::uint8_t,3> homing_order{0,1,2};
        std::array<HomeConfiguration,3> homing{};
    };

    // Deliberately empty until this machine's mechanics have been calibrated.
    // Simulation fixtures are never substituted for a missing physical config.
    [[nodiscard]] std::optional<RobotConfiguration> boardRobotConfiguration();
    [[nodiscard]] RobotConfiguration simulationConfiguration(const ::motion::trajectory::Capabilities& hardware,
                                                              bool archived_geometry = false);

    class RobotModel final
    {
      public:
        RobotModel(RobotConfiguration configuration, ::motion::trajectory::Capabilities hardware);
        [[nodiscard]] const RobotConfiguration& configuration() const noexcept { return m_configuration; }
        [[nodiscard]] const ScaraKinematics& kinematics() const noexcept { return m_kinematics; }
        [[nodiscard]] const auto& hardware() const noexcept { return m_hardware; }
        [[nodiscard]] RobotError physicalAdmission() const noexcept;
        [[nodiscard]] std::uint64_t fingerprint() const noexcept;
        [[nodiscard]] std::array<double,3> joints(JointPosition p) const noexcept;
        [[nodiscard]] JointPosition joints(const std::array<double,3>& p) const noexcept;
        [[nodiscard]] ::motion::trajectory::Positions toMotors(JointPosition joints) const;
        [[nodiscard]] JointPosition toJoints(const ::motion::trajectory::Positions& motors) const;
        [[nodiscard]] ::motion::trajectory::MotorStates toMotors(const std::array<::motion::KinematicState,3>& joints) const;
        [[nodiscard]] std::array<double,3> stepsPerJointUnit() const noexcept { return m_scale; }
        [[nodiscard]] std::array<::motion::trajectory::Limits,3> jointLimits() const noexcept;
        [[nodiscard]] std::array<::motion::trajectory::Limits,3> motorLimits() const noexcept;
        [[nodiscard]] CartesianPosition fromWork(CartesianPosition p) const noexcept;
        [[nodiscard]] CartesianPosition vectorFromWork(CartesianPosition p) const noexcept;
        [[nodiscard]] double quantizationBound() const noexcept;
      private:
        RobotConfiguration m_configuration;
        ::motion::trajectory::Capabilities m_hardware;
        std::array<RotaryAxisConversion,2> m_rotary;
        LinearAxisConversion m_linear;
        ScaraKinematics m_kinematics;
        std::array<double,3> m_scale{}, m_offset{};
    };
}
