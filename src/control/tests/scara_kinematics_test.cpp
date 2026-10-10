// Numeric literals are test inputs and independently specified expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "../AxisConversion.hpp"
#include "../ScaraKinematics.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace
{
    using Kinematics = ScaraKinematics;
    using enum Kinematics::Error;
    using enum Kinematics::ElbowBranch;
    using namespace pnm::units;
    using namespace pnm::units::literals;

    const Kinematics::Config BASIC{ .first_arm_length = 200_mm, .second_arm_length = 100_mm };

    template<IsQuantity Quantity>
    void expect_near(Quantity actual, Quantity expected, double tolerance = 1e-10)
    {
        EXPECT_NEAR(actual.get(), expected.get(), tolerance);
    }

    void expect_point(Kinematics::CartesianPosition actual, Kinematics::CartesianPosition expected)
    {
        expect_near(actual.x, expected.x);
        expect_near(actual.y, expected.y);
        expect_near(actual.z, expected.z);
    }

    void expect_joints(Kinematics::JointPosition actual, Kinematics::JointPosition expected)
    {
        expect_near(actual.shoulder, expected.shoulder);
        expect_near(actual.elbow, expected.elbow);
        expect_near(actual.z, expected.z);
    }
}

TEST(ScaraKinematics, ForwardUsesRelativeElbowAngleAndUpwardZ)
{
    const Kinematics kinematics{ BASIC };
    const auto straight{ kinematics.forward({ .shoulder = 0_deg, .elbow = 0_deg, .z = 10_mm }) };
    ASSERT_TRUE(straight);
    expect_point(straight->position, { .x = 300_mm, .y = 0_mm, .z = 10_mm });
    expect_near(straight->yaw, 0_deg);
    const auto right_angle{ kinematics.forward({ .shoulder = 0_deg, .elbow = 90_deg, .z = -10_mm }) };
    ASSERT_TRUE(right_angle);
    expect_point(right_angle->position, { .x = 200_mm, .y = 100_mm, .z = -10_mm });
    const auto folded{ kinematics.forward({ .shoulder = 90_deg, .elbow = -90_deg, .z = 0_mm }) };
    ASSERT_TRUE(folded);
    expect_point(folded->position, { .x = 100_mm, .y = 200_mm, .z = 0_mm });
    expect_near(folded->yaw, 0_deg);
}

TEST(ScaraKinematics, BaseAndToolFramesIncludeLateralOffsetWithoutRotatingItByToolYaw)
{
    auto config{ BASIC };
    config.base_origin = { .x = 10_mm, .y = 20_mm, .z = 30_mm };
    config.base_yaw = 90_deg;
    config.tool_offset = { .x = 30_mm, .y = 20_mm, .z = -10_mm };
    config.tool_yaw = 45_deg;
    const Kinematics kinematics{ config };
    const Kinematics::JointPosition joints{ .shoulder = 90_deg, .elbow = -90_deg, .z = 7_mm };
    const auto pose{ kinematics.forward(joints) };
    ASSERT_TRUE(pose);
    expect_point(pose->position, { .x = -210_mm, .y = 150_mm, .z = 27_mm });
    expect_near(pose->yaw, 135_deg);
    const auto inverse{ kinematics.inverse(pose->position, joints) };
    ASSERT_TRUE(inverse);
    expect_joints(*inverse, joints);
}

TEST(ScaraKinematics, InverseSolvesBothBranchesAndPreservesTheReferenceBranch)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::CartesianPosition target{ .x = 200_mm, .y = 100_mm, .z = 20_mm };
    const auto positive{ kinematics.inverse(target, { .shoulder = 0_deg, .elbow = 30_deg, .z = 0_mm }) };
    ASSERT_TRUE(positive);
    expect_joints(*positive, { .shoulder = 0_deg, .elbow = 90_deg, .z = 20_mm });
    const auto negative{ kinematics.inverse(target, { .shoulder = 0_deg, .elbow = -30_deg, .z = 0_mm }) };
    ASSERT_TRUE(negative);
    expect_joints(*negative, { .shoulder = 53.13010235415598_deg, .elbow = -90_deg, .z = 20_mm });
    const auto explicit_branch{ kinematics.inverse(target, *positive, Negative) };
    ASSERT_TRUE(explicit_branch);
    expect_joints(*explicit_branch, *negative);
    EXPECT_EQ(kinematics.elbowBranch(*positive), Positive);
    EXPECT_EQ(kinematics.elbowBranch(*negative), Negative);
}

TEST(ScaraKinematics, InverseKeepsUnwrappedTurnsAcrossTheMinusPiPiBoundary)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition expected{ .shoulder = 901_deg, .elbow = 420_deg, .z = 4_mm };
    const auto target{ kinematics.forward(expected) };
    ASSERT_TRUE(target);
    const auto result{ kinematics.inverse(target->position,
                                          { .shoulder = 899_deg, .elbow = 419_deg, .z = 4_mm }) };
    ASSERT_TRUE(result);
    expect_joints(*result, expected);
}

TEST(ScaraKinematics, InverseSelectsEquivalentTurnsInsideJointLimits)
{
    auto config{ BASIC };
    config.limits =
      Kinematics::JointLimits{ .minimum = { .shoulder = 700_deg, .elbow = 700_deg, .z = 0_mm },
                               .maximum = { .shoulder = 760_deg, .elbow = 800_deg, .z = 100_mm } };
    const Kinematics kinematics{ config };
    const Kinematics::JointPosition expected{ .shoulder = 740_deg, .elbow = 780_deg, .z = 30_mm };
    const auto target{ kinematics.forward(expected) };
    ASSERT_TRUE(target);
    // A reference outside limits is allowed; only the resulting target must fit.
    const auto result{ kinematics.inverse(target->position,
                                          { .shoulder = 20_deg, .elbow = 60_deg, .z = 0_mm }) };
    ASSERT_TRUE(result);
    expect_joints(*result, expected);
    EXPECT_TRUE(kinematics.checkJointLimits(*result));
}

TEST(ScaraKinematics, JointLimitsNeverCauseAnImplicitElbowBranchChange)
{
    auto config{ BASIC };
    config.limits =
      Kinematics::JointLimits{ .minimum = { .shoulder = -180_deg, .elbow = -170_deg, .z = 0_mm },
                               .maximum = { .shoulder = 180_deg, .elbow = -10_deg, .z = 100_mm } };
    const Kinematics kinematics{ config };
    const Kinematics::CartesianPosition target{ .x = 200_mm, .y = 100_mm, .z = 20_mm };
    const Kinematics::JointPosition reference{ .shoulder = 0_deg, .elbow = 30_deg, .z = 0_mm };
    EXPECT_EQ(kinematics.inverse(target, reference).error(), JointLimitExceeded);
    EXPECT_TRUE(kinematics.inverse(target, reference, Negative));
    // Forward feedback is still available outside configured limits.
    EXPECT_TRUE(kinematics.forward(reference));
    EXPECT_EQ(kinematics.checkJointLimits(reference).error(), JointLimitExceeded);
}

TEST(ScaraKinematics, LimitEndpointsAreInclusiveAndOutOfRangeTargetsAreRejected)
{
    auto config{ BASIC };
    config.base_origin.z = 123.4_mm;
    config.tool_offset.z = -15.6_mm;
    config.limits =
      Kinematics::JointLimits{ .minimum = { .shoulder = 0_deg, .elbow = 30_deg, .z = 0_mm },
                               .maximum = { .shoulder = 90_deg, .elbow = 90_deg, .z = 100_mm } };
    const Kinematics kinematics{ config };
    for (const auto joints :
         { config.limits->minimum,
           config.limits->maximum,
           Kinematics::JointPosition{ .shoulder = 0_deg, .elbow = 90_deg, .z = 100_mm } }) {
        const auto pose{ kinematics.forward(joints) };
        ASSERT_TRUE(pose);
        const auto result{ kinematics.inverse(pose->position, joints) };
        ASSERT_TRUE(result);
        expect_joints(*result, joints);
    }
    for (const auto joints :
         { Kinematics::JointPosition{ .shoulder = -1_deg, .elbow = 60_deg, .z = 50_mm },
           Kinematics::JointPosition{ .shoulder = 45_deg, .elbow = 100_deg, .z = 50_mm },
           Kinematics::JointPosition{ .shoulder = 45_deg, .elbow = 60_deg, .z = -0.1_mm },
           Kinematics::JointPosition{ .shoulder = 45_deg, .elbow = 60_deg, .z = 100.1_mm } }) {
        const auto pose{ kinematics.forward(joints) };
        ASSERT_TRUE(pose);
        EXPECT_EQ(kinematics.inverse(pose->position, joints).error(), JointLimitExceeded);
    }
}

TEST(ScaraKinematics, WorkspaceIncludesToolReachAndRejectsOuterAndInnerUnreachablePoints)
{
    const Kinematics basic{ BASIC };
    const Kinematics::JointPosition seed{ .shoulder = 0_deg, .elbow = 60_deg, .z = 0_mm };
    EXPECT_EQ(basic.inverse({ 300.01_mm, 0_mm, 0_mm }, seed).error(), Unreachable);
    EXPECT_EQ(basic.inverse({ 99.99_mm, 0_mm, 0_mm }, seed).error(), Unreachable);
    EXPECT_EQ(basic.inverse({ -400_mm, -400_mm, 0_mm }, seed).error(), Unreachable);
    auto config{ BASIC };
    config.tool_offset.x = 100_mm;
    const Kinematics longer{ config };
    const Kinematics::CartesianPosition target{ .x = 350_mm, .y = 0_mm, .z = 0_mm };
    const auto result{ longer.inverse(target, seed) };
    ASSERT_TRUE(result);
    const auto pose{ longer.forward(*result) };
    ASSERT_TRUE(pose);
    expect_point(pose->position, target);
}

TEST(ScaraKinematics, SingularEndpointsAreRejectedButForwardStateRemainsAvailable)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition seed{ .shoulder = 0_deg, .elbow = 60_deg, .z = 0_mm };
    EXPECT_EQ(kinematics.inverse({ 300_mm, 0_mm, 0_mm }, seed).error(), Singularity);
    EXPECT_EQ(kinematics.inverse({ 100_mm, 0_mm, 0_mm }, seed).error(), Singularity);
    EXPECT_TRUE(kinematics.forward({ 0_deg, 0_deg, 0_mm }));
    EXPECT_TRUE(kinematics.forwardVelocity({ 0_deg, 0_deg, 0_mm }, {}));
    EXPECT_EQ(kinematics.inverseVelocity({ 0_deg, 0_deg, 0_mm }, {}).error(), Singularity);
    EXPECT_EQ(kinematics.elbowBranch({ 0_deg, 180_deg, 0_mm }).error(), Singularity);
    EXPECT_EQ(kinematics.inverse({ 200_mm, 100_mm, 0_mm }, {}).error(), Singularity);
    EXPECT_TRUE(kinematics.inverse({ 200_mm, 100_mm, 0_mm }, {}, Positive));

    auto config{ BASIC };
    config.second_arm_length = 200_mm;
    const Kinematics equal_links{ config };
    EXPECT_EQ(equal_links.inverse({}, seed).error(), Singularity);
}

TEST(ScaraKinematics, SingularityThresholdUsesEffectiveBendIncludingLateralToolOffset)
{
    auto config{ BASIC };
    config.tool_offset.y = 100_mm; // Effective distal vector has a 45 degree phase.
    config.minimum_bend_sine = 0.01;
    const Kinematics kinematics{ config };
    const Kinematics::JointPosition singular{ .shoulder = 30_deg, .elbow = -45_deg, .z = 0_mm };
    EXPECT_EQ(kinematics.elbowBranch(singular).error(), Singularity);
    const auto pose{ kinematics.forward(singular) };
    ASSERT_TRUE(pose);
    EXPECT_EQ(kinematics.inverse(pose->position, {}, Positive).error(), Singularity);
    EXPECT_EQ(kinematics.inverseVelocity({ 30_deg, -44.9_deg, 0_mm }, {}).error(), Singularity);
    EXPECT_TRUE(kinematics.inverseVelocity({ 30_deg, -40_deg, 0_mm }, {}));
}

TEST(ScaraKinematics, ForwardAndInverseRoundTripAcrossBothBranchesFramesAndMultipleTurns)
{
    auto config{ BASIC };
    config.base_origin = { .x = -50_mm, .y = 40_mm, .z = 100_mm };
    config.base_yaw = 23_deg;
    config.tool_offset = { .x = 35_mm, .y = 20_mm, .z = -10_mm };
    config.tool_yaw = 17_deg;
    const Kinematics kinematics{ config };
    for (const auto shoulder : { -720_deg, -179_deg, -90_deg, 0_deg, 90_deg, 179_deg, 720_deg }) {
        for (const auto elbow : { -135_deg, -70_deg, -20_deg, 20_deg, 70_deg, 135_deg }) {
            const Kinematics::JointPosition joints{ .shoulder = shoulder, .elbow = elbow, .z = 45_mm };
            const auto pose{ kinematics.forward(joints) };
            ASSERT_TRUE(pose);
            const auto result{ kinematics.inverse(
              pose->position, { .shoulder = shoulder + 2_deg, .elbow = elbow, .z = 40_mm }) };
            ASSERT_TRUE(result);
            expect_joints(*result, joints);
        }
    }
}

TEST(ScaraKinematics, VelocityJacobianMatchesAnalyticExampleAndItsInverse)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition joints{ .shoulder = 0_deg, .elbow = 90_deg, .z = 10_mm };
    const Kinematics::JointVelocity rates{ .shoulder = 1_rad_s, .elbow = 2_rad_s, .z = -5_mm_s };
    const auto velocity{ kinematics.forwardVelocity(joints, rates) };
    ASSERT_TRUE(velocity);
    expect_near(velocity->linear.x, -300_mm_s);
    expect_near(velocity->linear.y, 200_mm_s);
    expect_near(velocity->linear.z, -5_mm_s);
    expect_near(velocity->yaw, 3_rad_s);
    const auto inverse{ kinematics.inverseVelocity(joints, velocity->linear) };
    ASSERT_TRUE(inverse);
    expect_near(inverse->shoulder, rates.shoulder);
    expect_near(inverse->elbow, rates.elbow);
    expect_near(inverse->z, rates.z);
}

TEST(ScaraKinematics, VelocityMatchesFiniteDifferenceOfForwardPositionWithToolAndBaseOffsets)
{
    auto config{ BASIC };
    config.base_origin = { .x = 20_mm, .y = 30_mm, .z = 40_mm };
    config.base_yaw = -47_deg;
    config.tool_offset = { .x = 45_mm, .y = -30_mm, .z = -20_mm };
    const Kinematics kinematics{ config };
    const Kinematics::JointVelocity velocity{ .shoulder = 12_rpm, .elbow = -8_rpm, .z = 7_mm_s };
    const auto dt{ Time::create(1e-5) };
    for (const auto elbow : { -130_deg, -40_deg, 30_deg, 120_deg }) {
        const Kinematics::JointPosition joints{ .shoulder = 400_deg, .elbow = elbow, .z = 10_mm };
        const auto before{ kinematics.forward({ .shoulder = joints.shoulder - velocity.shoulder * dt,
                                                .elbow = joints.elbow - velocity.elbow * dt,
                                                .z = joints.z - velocity.z * dt }) };
        const auto after{ kinematics.forward({ .shoulder = joints.shoulder + velocity.shoulder * dt,
                                               .elbow = joints.elbow + velocity.elbow * dt,
                                               .z = joints.z + velocity.z * dt }) };
        const auto derivative{ kinematics.forwardVelocity(joints, velocity) };
        ASSERT_TRUE(before);
        ASSERT_TRUE(after);
        ASSERT_TRUE(derivative);
        expect_near(derivative->linear.x, (after->position.x - before->position.x) / (2.0 * dt), 1e-8);
        expect_near(derivative->linear.y, (after->position.y - before->position.y) / (2.0 * dt), 1e-8);
        expect_near(derivative->linear.z, (after->position.z - before->position.z) / (2.0 * dt), 1e-8);
        expect_near(derivative->yaw, (after->yaw - before->yaw) / (2.0 * dt), 1e-8);
        const auto inverse{ kinematics.inverseVelocity(joints, derivative->linear) };
        ASSERT_TRUE(inverse);
        expect_near(inverse->shoulder, velocity.shoulder);
        expect_near(inverse->elbow, velocity.elbow);
        expect_near(inverse->z, velocity.z);
    }
}

TEST(ScaraKinematics, AxisConversionsComposeWithKinematicsInMotorAndJointUnits)
{
    const RotaryAxisConversion shoulder{ { .motor_revolutions_per_axis_revolution = 5.0,
                                           .direction = AxisDirection::OppositeToMotor,
                                           .motor_reference = 135_deg } };
    const RotaryAxisConversion elbow{ { .motor_revolutions_per_axis_revolution = 3.0,
                                        .motor_reference = 135_deg } };
    const LinearAxisConversion vertical{ { .travel_per_output_revolution = 40_mm,
                                           .motor_revolutions_per_output_revolution = 2.0,
                                           .motor_reference = 135_deg } };
    const Kinematics::JointPosition joints{ .shoulder = shoulder.toAxisPosition(-15_deg),
                                            .elbow = elbow.toAxisPosition(315_deg),
                                            .z = vertical.toAxisPosition(315_deg) };
    const Kinematics kinematics{ BASIC };
    const auto pose{ kinematics.forward(joints) };
    ASSERT_TRUE(pose);
    expect_point(pose->position, { .x = 173.20508075688772_mm, .y = 200_mm, .z = 10_mm });
    const auto inverse{ kinematics.inverse(pose->position, joints) };
    ASSERT_TRUE(inverse);
    expect_near(shoulder.toMotorPosition(inverse->shoulder), -15_deg);
    expect_near(elbow.toMotorPosition(inverse->elbow), 315_deg);
    expect_near(vertical.toMotorPosition(inverse->z), 315_deg);
}

TEST(ScaraKinematics, InvalidConfigurationIsRejected)
{
    for (const auto value :
         { 0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        auto config{ BASIC };
        config.first_arm_length = Distance::create(value);
        EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
        config = BASIC;
        config.second_arm_length = Distance::create(value);
        EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
        config = BASIC;
        config.minimum_bend_sine = value;
        EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
    }
    auto config{ BASIC };
    config.minimum_bend_sine = 1.0;
    EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
    config = BASIC;
    config.tool_offset.x = -100_mm; // Tool coincides with elbow: no second positional degree of freedom.
    EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
    config = BASIC;
    config.base_yaw = Angle::create(std::numeric_limits<double>::quiet_NaN());
    EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
    config = BASIC;
    config.tool_offset.z = Distance::create(std::numeric_limits<double>::infinity());
    EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
    config = BASIC;
    config.limits = Kinematics::JointLimits{ .minimum = { .shoulder = 90_deg, .elbow = 0_deg, .z = 0_mm },
                                             .maximum = { .shoulder = 0_deg, .elbow = 90_deg, .z = 10_mm } };
    EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
}

TEST(ScaraKinematics, NonfiniteInputsInvalidBranchesAndOverflowAreReported)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition seed{ .shoulder = 0_deg, .elbow = 90_deg, .z = 0_mm };
    const Kinematics::CartesianPosition target{ .x = 200_mm, .y = 100_mm, .z = 0_mm };
    for (const auto value :
         { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        const Kinematics::JointPosition bad{ .shoulder = Angle::create(value), .elbow = 90_deg, .z = 0_mm };
        EXPECT_EQ(kinematics.forward(bad).error(), InvalidInput);
        EXPECT_EQ(kinematics.elbowBranch(bad).error(), InvalidInput);
        EXPECT_EQ(kinematics.checkJointLimits(bad).error(), InvalidInput);
        EXPECT_EQ(kinematics.inverse(target, bad).error(), InvalidInput);
        EXPECT_EQ(kinematics.inverse({ Distance::create(value), 100_mm, 0_mm }, seed).error(), InvalidInput);
        EXPECT_EQ(kinematics.forwardVelocity(seed, { AngularVelocity::create(value), 0_rpm, 0_mm_s }).error(),
                  InvalidInput);
        EXPECT_EQ(kinematics.inverseVelocity(seed, { Velocity::create(value), 0_mm_s, 0_mm_s }).error(),
                  InvalidInput);
    }
    // Deliberately exercise rejection of a non-enumerator value.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_EQ(kinematics.inverse(target, seed, static_cast<Kinematics::ElbowBranch>(99)).error(),
              InvalidInput);
    const auto maximum{ std::numeric_limits<double>::max() };
    EXPECT_EQ(
      kinematics
        .forwardVelocity(seed, { AngularVelocity::create(maximum), AngularVelocity::create(maximum), 0_mm_s })
        .error(),
      NumericOverflow);
    auto config{ BASIC };
    config.base_origin.z = Distance::create(maximum);
    const Kinematics large_origin{ config };
    EXPECT_EQ(large_origin.forward({ 0_deg, 90_deg, Distance::create(maximum) }).error(), NumericOverflow);
    EXPECT_EQ(large_origin.inverse({ 200_mm, 100_mm, Distance::create(-maximum) }, seed).error(),
              NumericOverflow);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
