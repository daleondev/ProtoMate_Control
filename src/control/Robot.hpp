#pragma once

#include "MotionController.hpp"
#include "ScaraKinematics.hpp"

namespace control
{
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
            MotionController::MotorId motor;
            std::error_code reason;
        };
        struct Status
        {
            MotionController::Status motors;
            State commanded;
            // All three axes must have valid position AND velocity feedback.
            // Never substitute pulse counts for an absent or faulted encoder.
            std::expected<State, FeedbackError> actual;
            bool referenced;
        };

        Robot(std::shared_ptr<MotionController> controller, ScaraKinematics::Config geometry);
        Robot(std::shared_ptr<MotionController> controller,
              ScaraKinematics::Config geometry,
              ScaraKinematics::JointVelocity maximum_velocity);

        struct MoveOptions
        {
            double speed{ 0.2 }; // Fraction of configured joint speed limits: (0, 1].
            pnm::units::Time timeout{ 0_s };
            std::optional<ScaraKinematics::ElbowBranch> branch{};
        };
        using Motion = MotionController::GroupMotion;
        // Rest-to-rest, synchronized linear interpolation in JOINT coordinates.
        // Cartesian targets use IK first; their tool path can curve. Every move
        // requires all axes referenced and idle. No implicit abort or buffering.
        Motion moveAbs(ScaraKinematics::CartesianPosition target, MoveOptions options = { 0.2, 0_s, {} });
        Motion moveRel(ScaraKinematics::CartesianPosition distance, MoveOptions options = { 0.2, 0_s, {} });
        Motion moveJoints(ScaraKinematics::JointPosition target, MoveOptions options = { 0.2, 0_s, {} });
        Motion reference(pnm::units::Time timeout = 90_s);
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
        MotionController::CoordinatedPlan plan(const MotionController::Status& origin,
                                               const std::array<StepperMotor::MotionDefaults, 3>& defaults,
                                               ScaraKinematics::JointPosition target,
                                               double speed) const;
        const std::shared_ptr<MotionController> m_controller;
        const ScaraKinematics m_kinematics;
        const ScaraKinematics::JointVelocity m_maximumVelocity;
    };
}
