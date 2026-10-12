#pragma once

#include "ControllerClient.hpp"
#include "ScaraKinematics.hpp"

namespace control
{
    inline constexpr double DEFAULT_ROBOT_SPEED{ 0.2 };

    // Combines the application's existing axes with its SCARA geometry. Owns
    // no additional peripherals, worker or cached position. Thread-context only.
    class Robot final
    {
      public:
        struct State
        {
            ScaraKinematics::JointPosition joints;
            ScaraKinematics::JointVelocity joint_velocity;
            ScaraKinematics::Result<ScaraKinematics::ToolPose> pose;
            ScaraKinematics::Result<ScaraKinematics::ToolVelocity> velocity;
        };
        struct FeedbackError
        {
            ControllerClient::MotorId motor;
            std::error_code reason;
        };
        struct Status
        {
            ControllerClient::Status motors;
            State commanded;
            // All three axes must have valid position AND velocity feedback.
            // Never substitute pulse counts for an absent or faulted encoder.
            std::expected<State, FeedbackError> actual;
            bool referenced;
        };

        Robot(std::shared_ptr<ControllerClient> controller, ScaraKinematics::Config geometry);
        Robot(std::shared_ptr<ControllerClient> controller,
              ScaraKinematics::Config geometry,
              ScaraKinematics::JointVelocity maximum_velocity);

        struct MoveOptions
        {
            double speed{ DEFAULT_ROBOT_SPEED }; // Fraction of configured joint speed limits: (0, 1].
            pnm::units::Time timeout{ 0_s };
            std::optional<ScaraKinematics::ElbowBranch> branch{};
        };
        using Motion = ControllerClient::GroupMotion;
        // Rest-to-rest, synchronized linear interpolation in JOINT coordinates.
        // Cartesian targets use IK first; their tool path can curve. Every move
        // requires all axes referenced and idle. No implicit abort or buffering.
        Motion moveAbs(ScaraKinematics::CartesianPosition target,
                       MoveOptions options = { .speed = DEFAULT_ROBOT_SPEED, .timeout = 0_s, .branch = {} });
        Motion moveRel(ScaraKinematics::CartesianPosition distance,
                       MoveOptions options = { .speed = DEFAULT_ROBOT_SPEED, .timeout = 0_s, .branch = {} });
        Motion moveJoints(ScaraKinematics::JointPosition target,
                          MoveOptions options = { .speed = DEFAULT_ROBOT_SPEED,
                                                  .timeout = 0_s,
                                                  .branch = {} });
        Motion reference(pnm::units::Time timeout = control::DEFAULT_GROUP_REFERENCE_TIMEOUT);
        void enable();
        void disable();
        void stop(); // Immediate group abort; holding torque remains enabled.
        void reset();
        std::vector<Motion> motions();
        ScaraKinematics::JointVelocity maximumVelocity() const noexcept { return m_maximumVelocity; }

        const ScaraKinematics& kinematics() const noexcept { return m_kinematics; }
        // Uses the conversions from the same controller snapshot as the motor
        // readings. Readings are sampled sequentially, not hardware-latched.
        // Unreferenced commanded coordinates remain available for diagnostics.
        // Throws if mechanics are missing or a conversion is unrepresentable.
        Status status() const;

      private:
        ControllerClient::Trajectory plan(const ControllerClient::PlanningSnapshot& snapshot,
                                          ScaraKinematics::JointPosition target,
                                          double speed) const;
        std::shared_ptr<ControllerClient> m_controller;
        ScaraKinematics m_kinematics;
        ScaraKinematics::JointVelocity m_maximumVelocity;
    };
}
