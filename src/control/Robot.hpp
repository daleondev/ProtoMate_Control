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

        const ScaraKinematics& kinematics() const noexcept { return m_kinematics; }
        // Uses the conversions from the same controller snapshot as the motor
        // readings. Readings are sampled sequentially, not hardware-latched.
        // Unreferenced commanded coordinates remain available for diagnostics.
        // Throws if mechanics are missing or a conversion is unrepresentable.
        Status status() const;

      private:
        const std::shared_ptr<MotionController> m_controller;
        const ScaraKinematics m_kinematics;
    };
}
