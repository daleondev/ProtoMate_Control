#include "ScaraKinematics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace
{
    using Kinematics = ScaraKinematics;
    using Error = Kinematics::Error;
    using namespace pnm::units;

    constexpr double TURN{ 2.0 * std::numbers::pi };
    constexpr double ROUNDING_TOLERANCE{ 64.0 * std::numeric_limits<double>::epsilon() };

    template<IsQuantity... Quantities>
    bool finite(Quantities... values) noexcept
    {
        return (values.isFinite() && ...);
    }

    bool finite(Kinematics::JointPosition joints) noexcept
    {
        return finite(joints.shoulder, joints.elbow, joints.z);
    }

    bool finite(Kinematics::CartesianPosition point) noexcept { return finite(point.x, point.y, point.z); }

    double radians(Angle angle) noexcept { return std::remainder(angle.get(), TURN); }

    Kinematics::Result<Angle> nearestEquivalent(Angle angle,
                                                Angle reference,
                                                std::optional<std::pair<Angle, Angle>> limits) noexcept
    {
        // Divide first to avoid overflow when finite reference and angle have
        // opposite signs. Do not wrap the final answer back into +/- pi.
        auto turns{ std::round(reference.get() / TURN - angle.get() / TURN) };
        if (limits) {
            const auto [minimum, maximum]{ *limits };
            const auto lower{ minimum.get() / TURN - angle.get() / TURN };
            const auto upper{ maximum.get() / TURN - angle.get() / TURN };
            const auto tolerance{ ROUNDING_TOLERANCE * std::max({ 1.0, std::abs(lower), std::abs(upper) }) };
            const auto first{ std::ceil(lower - tolerance) };
            const auto last{ std::floor(upper + tolerance) };
            if (first > last) {
                return std::unexpected(Error::JointLimitExceeded);
            }
            turns = std::clamp(turns, first, last);
        }
        auto result{ std::fma(turns, TURN, angle.get()) };
        if (!std::isfinite(result)) {
            return std::unexpected(Error::NumericOverflow);
        }
        if (limits) {
            const auto [minimum, maximum]{ *limits };
            const auto tolerance{ ROUNDING_TOLERANCE *
                                  std::max({ 1.0, std::abs(minimum.get()), std::abs(maximum.get()) }) };
            if (result < minimum.get() - tolerance || result > maximum.get() + tolerance) {
                return std::unexpected(Error::JointLimitExceeded);
            }
            // Only absorb floating-point roundoff at a valid limit endpoint.
            result = std::clamp(result, minimum.get(), maximum.get());
        }
        return Angle::create(result);
    }

    struct ArmVectors
    {
        double first_x, first_y, second_x, second_y;
    };

    ArmVectors vectors(Kinematics::JointPosition joints,
                       Angle base_yaw,
                       double first_length,
                       double second_length,
                       double tool_phase) noexcept
    {
        const auto shoulder{ radians(base_yaw) + radians(joints.shoulder) };
        const auto distal{ shoulder + radians(joints.elbow) + tool_phase };
        return { first_length * std::cos(shoulder),
                 first_length * std::sin(shoulder),
                 second_length * std::cos(distal),
                 second_length * std::sin(distal) };
    }
}

ScaraKinematics::ScaraKinematics(Config config)
  : m_config{ std::move(config) }
{
    if (!finite(
          m_config.first_arm_length, m_config.second_arm_length, m_config.base_yaw, m_config.tool_yaw) ||
        m_config.first_arm_length.get() <= 0.0 || m_config.second_arm_length.get() <= 0.0 ||
        !finite(m_config.base_origin) || !finite(m_config.tool_offset) ||
        !std::isfinite(m_config.minimum_bend_sine) || m_config.minimum_bend_sine <= 0.0 ||
        m_config.minimum_bend_sine >= 1.0) {
        throw std::invalid_argument("invalid SCARA geometry or singularity threshold");
    }
    m_firstLength = m_config.first_arm_length.get();
    const auto forward_offset{ (m_config.second_arm_length + m_config.tool_offset.x).get() };
    const auto lateral_offset{ m_config.tool_offset.y.get() };
    m_effectiveSecondLength = std::hypot(forward_offset, lateral_offset);
    m_toolPhase = std::atan2(lateral_offset, forward_offset);
    const auto scale{ std::max(m_firstLength, m_effectiveSecondLength) };
    const auto normalized_product{ (m_firstLength / scale) * (m_effectiveSecondLength / scale) };
    if (!std::isfinite(m_effectiveSecondLength) || m_effectiveSecondLength <= 0.0 ||
        !std::isfinite(m_firstLength + m_effectiveSecondLength) || normalized_product == 0.0) {
        throw std::invalid_argument("degenerate or unrepresentable SCARA link lengths");
    }
    if (m_config.limits) {
        const auto& [minimum, maximum]{ *m_config.limits };
        if (!finite(minimum) || !finite(maximum) || minimum.shoulder > maximum.shoulder ||
            minimum.elbow > maximum.elbow || minimum.z > maximum.z) {
            throw std::invalid_argument("invalid SCARA joint limits");
        }
    }
}

ScaraKinematics::Result<ScaraKinematics::ToolPose> ScaraKinematics::forward(
  JointPosition joints) const noexcept
{
    if (!finite(joints)) {
        return std::unexpected(Error::InvalidInput);
    }
    const auto arm{ vectors(joints, m_config.base_yaw, m_firstLength, m_effectiveSecondLength, m_toolPhase) };
    ToolPose pose{ { m_config.base_origin.x + Distance::create(arm.first_x + arm.second_x),
                     m_config.base_origin.y + Distance::create(arm.first_y + arm.second_y),
                     m_config.base_origin.z + joints.z + m_config.tool_offset.z },
                   m_config.base_yaw + joints.shoulder + joints.elbow + m_config.tool_yaw };
    if (!finite(pose.position) || !pose.yaw.isFinite()) {
        return std::unexpected(Error::NumericOverflow);
    }
    return pose;
}

ScaraKinematics::Result<ScaraKinematics::ElbowBranch> ScaraKinematics::elbowBranch(
  JointPosition joints) const noexcept
{
    if (!finite(joints)) {
        return std::unexpected(Error::InvalidInput);
    }
    const auto sine{ std::sin(radians(joints.elbow) + m_toolPhase) };
    if (std::abs(sine) <= m_config.minimum_bend_sine) {
        return std::unexpected(Error::Singularity);
    }
    return sine > 0.0 ? ElbowBranch::Positive : ElbowBranch::Negative;
}

ScaraKinematics::Result<> ScaraKinematics::checkJointLimits(JointPosition joints) const noexcept
{
    if (!finite(joints)) {
        return std::unexpected(Error::InvalidInput);
    }
    if (m_config.limits) {
        const auto& [minimum, maximum]{ *m_config.limits };
        if (joints.shoulder < minimum.shoulder || joints.shoulder > maximum.shoulder ||
            joints.elbow < minimum.elbow || joints.elbow > maximum.elbow || joints.z < minimum.z ||
            joints.z > maximum.z) {
            return std::unexpected(Error::JointLimitExceeded);
        }
    }
    return {};
}

ScaraKinematics::Result<ScaraKinematics::JointPosition> ScaraKinematics::inverse(
  CartesianPosition target,
  JointPosition reference,
  std::optional<ElbowBranch> branch) const noexcept
{
    if (!finite(target) || !finite(reference)) {
        return std::unexpected(Error::InvalidInput);
    }
    if (!branch) {
        const auto current_branch{ elbowBranch(reference) };
        if (!current_branch) {
            return std::unexpected(current_branch.error());
        }
        branch = *current_branch;
    }
    if (*branch != ElbowBranch::Positive && *branch != ElbowBranch::Negative) {
        return std::unexpected(Error::InvalidInput);
    }

    const auto dx{ (target.x - m_config.base_origin.x).get() };
    const auto dy{ (target.y - m_config.base_origin.y).get() };
    const auto z{ target.z - m_config.base_origin.z - m_config.tool_offset.z };
    const auto radius{ std::hypot(dx, dy) };
    if (!std::isfinite(radius) || !z.isFinite()) {
        return std::unexpected(Error::NumericOverflow);
    }
    const auto scale{ std::max(m_firstLength, m_effectiveSecondLength) };
    const auto first{ m_firstLength / scale };
    const auto second{ m_effectiveSecondLength / scale };
    const auto r{ radius / scale };
    if (r > first + second + ROUNDING_TOLERANCE || r < std::abs(first - second) - ROUNDING_TOLERANCE) {
        return std::unexpected(Error::Unreachable);
    }
    // Scaling avoids squaring potentially large lengths. Clamp only roundoff
    // at the workspace boundary; genuinely unreachable targets were rejected.
    const auto cosine{ std::clamp(
      (r * r - first * first - second * second) / (2.0 * first * second), -1.0, 1.0) };
    const auto sine{ std::sqrt(std::max(0.0, (1.0 - cosine) * (1.0 + cosine))) };
    if (sine <= m_config.minimum_bend_sine) {
        return std::unexpected(Error::Singularity);
    }
    const auto bend{ (*branch == ElbowBranch::Positive ? 1.0 : -1.0) * std::atan2(sine, cosine) };
    const auto shoulder{ Angle::create(
      std::atan2(dy, dx) - radians(m_config.base_yaw) -
      std::atan2(second * std::sin(bend), first + second * std::cos(bend))) };
    const auto elbow{ Angle::create(bend - m_toolPhase) };
    const auto shoulder_limits{ m_config.limits
                                  ? std::optional{ std::pair{ m_config.limits->minimum.shoulder,
                                                              m_config.limits->maximum.shoulder } }
                                  : std::nullopt };
    const auto elbow_limits{ m_config.limits ? std::optional{ std::pair{ m_config.limits->minimum.elbow,
                                                                         m_config.limits->maximum.elbow } }
                                             : std::nullopt };
    const auto fitted_shoulder{ nearestEquivalent(shoulder, reference.shoulder, shoulder_limits) };
    const auto fitted_elbow{ nearestEquivalent(elbow, reference.elbow, elbow_limits) };
    if (!fitted_shoulder) {
        return std::unexpected(fitted_shoulder.error());
    }
    if (!fitted_elbow) {
        return std::unexpected(fitted_elbow.error());
    }
    auto fitted_z{ z };
    if (m_config.limits) {
        const auto minimum{ m_config.limits->minimum.z.get() };
        const auto maximum{ m_config.limits->maximum.z.get() };
        const auto tolerance{ ROUNDING_TOLERANCE * std::max({ 1.0,
                                                              std::abs(target.z.get()),
                                                              std::abs(m_config.base_origin.z.get()),
                                                              std::abs(m_config.tool_offset.z.get()),
                                                              std::abs(minimum),
                                                              std::abs(maximum) }) };
        if (z.get() < minimum - tolerance || z.get() > maximum + tolerance) {
            return std::unexpected(Error::JointLimitExceeded);
        }
        fitted_z = Distance::create(std::clamp(z.get(), minimum, maximum));
    }
    const JointPosition result{ *fitted_shoulder, *fitted_elbow, fitted_z };
    if (const auto check{ checkJointLimits(result) }; !check) {
        return std::unexpected(check.error());
    }
    return result;
}

ScaraKinematics::Result<ScaraKinematics::ToolVelocity> ScaraKinematics::forwardVelocity(
  JointPosition joints,
  JointVelocity velocity) const noexcept
{
    if (!finite(joints) || !finite(velocity.shoulder, velocity.elbow, velocity.z)) {
        return std::unexpected(Error::InvalidInput);
    }
    const auto arm{ vectors(joints, m_config.base_yaw, m_firstLength, m_effectiveSecondLength, m_toolPhase) };
    const auto shoulder_rate{ velocity.shoulder.get() };
    const auto distal_rate{ (velocity.shoulder + velocity.elbow).get() };
    ToolVelocity result{ { Velocity::create(-arm.first_y * shoulder_rate - arm.second_y * distal_rate),
                           Velocity::create(arm.first_x * shoulder_rate + arm.second_x * distal_rate),
                           velocity.z },
                         AngularVelocity::create(distal_rate) };
    if (!finite(result.linear.x, result.linear.y, result.linear.z, result.yaw)) {
        return std::unexpected(Error::NumericOverflow);
    }
    return result;
}

ScaraKinematics::Result<ScaraKinematics::JointVelocity> ScaraKinematics::inverseVelocity(
  JointPosition joints,
  CartesianVelocity velocity) const noexcept
{
    if (!finite(velocity.x, velocity.y, velocity.z)) {
        return std::unexpected(Error::InvalidInput);
    }
    if (const auto check{ checkJointLimits(joints) }; !check) {
        return std::unexpected(check.error());
    }
    if (const auto branch{ elbowBranch(joints) }; !branch) {
        return std::unexpected(branch.error());
    }
    // Express tool velocity in the first arm's frame and solve the planar
    // Jacobian. Tool yaw follows the two joint rates; it is not a fourth input.
    const auto shoulder{ radians(m_config.base_yaw) + radians(joints.shoulder) };
    const auto bend{ radians(joints.elbow) + m_toolPhase };
    const auto vx{ std::cos(shoulder) * velocity.x.get() + std::sin(shoulder) * velocity.y.get() };
    const auto vy{ -std::sin(shoulder) * velocity.x.get() + std::cos(shoulder) * velocity.y.get() };
    const auto sine{ std::sin(bend) };
    const auto shoulder_rate{ (vy + std::cos(bend) / sine * vx) / m_firstLength };
    const auto distal_rate{ -vx / m_effectiveSecondLength / sine };
    JointVelocity result{ AngularVelocity::create(shoulder_rate),
                          AngularVelocity::create(distal_rate - shoulder_rate),
                          velocity.z };
    if (!finite(result.shoulder, result.elbow, result.z)) {
        return std::unexpected(Error::NumericOverflow);
    }
    return result;
}
