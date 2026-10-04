#pragma once

#include "pneumo/units.hpp"

#include <expected>
#include <optional>

// Geometry of a serial shoulder/elbow/Z robot. Angles are joint angles after
// AxisConversion; elbow is relative to the first arm. Positive Z points up.
// No motor access, pulse rounding, trajectory planning or homing state.
class ScaraKinematics final
{
  public:
    struct JointPosition
    {
        pnm::units::Angle shoulder{};
        pnm::units::Angle elbow{};
        pnm::units::Distance z{};
    };

    struct CartesianPosition
    {
        pnm::units::Distance x{};
        pnm::units::Distance y{};
        pnm::units::Distance z{};
    };

    struct ToolPose
    {
        CartesianPosition position{};
        // Derived orientation; this robot cannot command yaw independently.
        pnm::units::Angle yaw{};
    };

    struct JointVelocity
    {
        pnm::units::AngularVelocity shoulder{};
        pnm::units::AngularVelocity elbow{};
        pnm::units::Velocity z{};
    };

    struct CartesianVelocity
    {
        pnm::units::Velocity x{};
        pnm::units::Velocity y{};
        pnm::units::Velocity z{};
    };

    struct ToolVelocity
    {
        CartesianVelocity linear{};
        pnm::units::AngularVelocity yaw{};
    };

    struct JointLimits
    {
        // Inclusive unwrapped coordinates. Angles may span multiple turns.
        JointPosition minimum{};
        JointPosition maximum{};
    };

    struct Config
    {
        pnm::units::Distance first_arm_length{};  // Shoulder axis to elbow axis.
        pnm::units::Distance second_arm_length{}; // Elbow axis to Z attachment datum.
        // World location of the shoulder frame. At shoulder=0 the first arm
        // points along this frame's +X; positive angles are CCW viewed from +Z.
        CartesianPosition base_origin{};
        pnm::units::Angle base_yaw{};
        // Offset from the Z attachment datum to the tool tip, expressed in the
        // second arm's frame: +X forward, +Y left, +Z up. Independent of tool_yaw.
        CartesianPosition tool_offset{};
        pnm::units::Angle tool_yaw{};
        std::optional<JointLimits> limits{};
        // Reject inverse solutions with |sin(effective elbow bend)| <= this.
        // This is a dimensionless singularity threshold, not a velocity limit.
        double minimum_bend_sine{ 1e-6 };
    };

    enum class ElbowBranch
    {
        // Sign of sin(elbow + atan2(tool_offset.y, second_arm_length + tool_offset.x)).
        // For a tool on the second arm's centreline these are the usual two bends.
        Positive,
        Negative
    };

    enum class Error
    {
        InvalidInput,
        Unreachable,
        JointLimitExceeded,
        Singularity,
        NumericOverflow
    };

    template<typename T = void>
    using Result = std::expected<T, Error>;

    // Throws invalid_argument for invalid/nonfinite/degenerate configuration.
    explicit ScaraKinematics(Config config);
    const Config& configuration() const noexcept { return m_config; }

    // Forward calculations allow positions outside limits and at singularities
    // so measured state can always be reported when its numbers are finite.
    Result<ToolPose> forward(JointPosition joints) const noexcept;
    Result<ToolVelocity> forwardVelocity(JointPosition joints, JointVelocity velocity) const noexcept;

    // Default: preserve reference's branch. An explicit branch permits selecting
    // a branch even from a singular reference. Never silently try the other branch.
    // Select the nearest equivalent angles to reference that satisfy the limits.
    // This validates an endpoint only; it does not prove a path is collision-free.
    Result<JointPosition> inverse(CartesianPosition target,
                                  JointPosition reference,
                                  std::optional<ElbowBranch> branch = std::nullopt) const noexcept;
    Result<JointVelocity> inverseVelocity(JointPosition joints, CartesianVelocity velocity) const noexcept;

    Result<ElbowBranch> elbowBranch(JointPosition joints) const noexcept;
    Result<> checkJointLimits(JointPosition joints) const noexcept;

  private:
    Config m_config;
    double m_firstLength{};
    double m_effectiveSecondLength{};
    double m_toolPhase{};
};
