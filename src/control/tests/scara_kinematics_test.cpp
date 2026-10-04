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
    void expectNear(Quantity actual, Quantity expected, double tolerance = 1e-10)
    {
        EXPECT_NEAR(actual.get(), expected.get(), tolerance);
    }

    void expectPoint(Kinematics::CartesianPosition actual, Kinematics::CartesianPosition expected)
    {
        expectNear(actual.x, expected.x);
        expectNear(actual.y, expected.y);
        expectNear(actual.z, expected.z);
    }

    void expectJoints(Kinematics::JointPosition actual, Kinematics::JointPosition expected)
    {
        expectNear(actual.shoulder, expected.shoulder);
        expectNear(actual.elbow, expected.elbow);
        expectNear(actual.z, expected.z);
    }
}

TEST(ScaraKinematics, ForwardUsesRelativeElbowAngleAndUpwardZ)
{
    const Kinematics kinematics{ BASIC };
    const auto straight{ kinematics.forward({ 0_deg, 0_deg, 10_mm }) };
    ASSERT_TRUE(straight);
    expectPoint(straight->position, { 300_mm, 0_mm, 10_mm });
    expectNear(straight->yaw, 0_deg);
    const auto right_angle{ kinematics.forward({ 0_deg, 90_deg, -10_mm }) };
    ASSERT_TRUE(right_angle);
    expectPoint(right_angle->position, { 200_mm, 100_mm, -10_mm });
    const auto folded{ kinematics.forward({ 90_deg, -90_deg, 0_mm }) };
    ASSERT_TRUE(folded);
    expectPoint(folded->position, { 100_mm, 200_mm, 0_mm });
    expectNear(folded->yaw, 0_deg);
}

TEST(ScaraKinematics, BaseAndToolFramesIncludeLateralOffsetWithoutRotatingItByToolYaw)
{
    auto config{ BASIC };
    config.base_origin = { 10_mm, 20_mm, 30_mm };
    config.base_yaw = 90_deg;
    config.tool_offset = { 30_mm, 20_mm, -10_mm };
    config.tool_yaw = 45_deg;
    const Kinematics kinematics{ config };
    const Kinematics::JointPosition joints{ 90_deg, -90_deg, 7_mm };
    const auto pose{ kinematics.forward(joints) };
    ASSERT_TRUE(pose);
    expectPoint(pose->position, { -210_mm, 150_mm, 27_mm });
    expectNear(pose->yaw, 135_deg);
    const auto inverse{ kinematics.inverse(pose->position, joints) };
    ASSERT_TRUE(inverse);
    expectJoints(*inverse, joints);
}

TEST(ScaraKinematics, InverseSolvesBothBranchesAndPreservesTheReferenceBranch)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::CartesianPosition target{ 200_mm, 100_mm, 20_mm };
    const auto positive{ kinematics.inverse(target, { 0_deg, 30_deg, 0_mm }) };
    ASSERT_TRUE(positive);
    expectJoints(*positive, { 0_deg, 90_deg, 20_mm });
    const auto negative{ kinematics.inverse(target, { 0_deg, -30_deg, 0_mm }) };
    ASSERT_TRUE(negative);
    expectJoints(*negative, { 53.13010235415598_deg, -90_deg, 20_mm });
    const auto explicit_branch{ kinematics.inverse(target, *positive, Negative) };
    ASSERT_TRUE(explicit_branch);
    expectJoints(*explicit_branch, *negative);
    EXPECT_EQ(kinematics.elbowBranch(*positive), Positive);
    EXPECT_EQ(kinematics.elbowBranch(*negative), Negative);
}

TEST(ScaraKinematics, InverseKeepsUnwrappedTurnsAcrossTheMinusPiPiBoundary)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition expected{ 901_deg, 420_deg, 4_mm };
    const auto target{ kinematics.forward(expected) };
    ASSERT_TRUE(target);
    const auto result{ kinematics.inverse(target->position, { 899_deg, 419_deg, 4_mm }) };
    ASSERT_TRUE(result);
    expectJoints(*result, expected);
}

TEST(ScaraKinematics, InverseSelectsEquivalentTurnsInsideJointLimits)
{
    auto config{ BASIC };
    config.limits = Kinematics::JointLimits{ { 700_deg, 700_deg, 0_mm }, { 760_deg, 800_deg, 100_mm } };
    const Kinematics kinematics{ config };
    const Kinematics::JointPosition expected{ 740_deg, 780_deg, 30_mm };
    const auto target{ kinematics.forward(expected) };
    ASSERT_TRUE(target);
    // A reference outside limits is allowed; only the resulting target must fit.
    const auto result{ kinematics.inverse(target->position, { 20_deg, 60_deg, 0_mm }) };
    ASSERT_TRUE(result);
    expectJoints(*result, expected);
    EXPECT_TRUE(kinematics.checkJointLimits(*result));
}

TEST(ScaraKinematics, JointLimitsNeverCauseAnImplicitElbowBranchChange)
{
    auto config{ BASIC };
    config.limits = Kinematics::JointLimits{ { -180_deg, -170_deg, 0_mm }, { 180_deg, -10_deg, 100_mm } };
    const Kinematics kinematics{ config };
    const Kinematics::CartesianPosition target{ 200_mm, 100_mm, 20_mm };
    const Kinematics::JointPosition reference{ 0_deg, 30_deg, 0_mm };
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
    config.limits = Kinematics::JointLimits{ { 0_deg, 30_deg, 0_mm }, { 90_deg, 90_deg, 100_mm } };
    const Kinematics kinematics{ config };
    for (const auto joints : { config.limits->minimum,
                               config.limits->maximum,
                               Kinematics::JointPosition{ 0_deg, 90_deg, 100_mm } }) {
        const auto pose{ kinematics.forward(joints) };
        ASSERT_TRUE(pose);
        const auto result{ kinematics.inverse(pose->position, joints) };
        ASSERT_TRUE(result);
        expectJoints(*result, joints);
    }
    for (const auto joints : { Kinematics::JointPosition{ -1_deg, 60_deg, 50_mm },
                               Kinematics::JointPosition{ 45_deg, 100_deg, 50_mm },
                               Kinematics::JointPosition{ 45_deg, 60_deg, -0.1_mm },
                               Kinematics::JointPosition{ 45_deg, 60_deg, 100.1_mm } }) {
        const auto pose{ kinematics.forward(joints) };
        ASSERT_TRUE(pose);
        EXPECT_EQ(kinematics.inverse(pose->position, joints).error(), JointLimitExceeded);
    }
}

TEST(ScaraKinematics, WorkspaceIncludesToolReachAndRejectsOuterAndInnerUnreachablePoints)
{
    const Kinematics basic{ BASIC };
    const Kinematics::JointPosition seed{ 0_deg, 60_deg, 0_mm };
    EXPECT_EQ(basic.inverse({ 300.01_mm, 0_mm, 0_mm }, seed).error(), Unreachable);
    EXPECT_EQ(basic.inverse({ 99.99_mm, 0_mm, 0_mm }, seed).error(), Unreachable);
    EXPECT_EQ(basic.inverse({ -400_mm, -400_mm, 0_mm }, seed).error(), Unreachable);
    auto config{ BASIC };
    config.tool_offset.x = 100_mm;
    const Kinematics longer{ config };
    const Kinematics::CartesianPosition target{ 350_mm, 0_mm, 0_mm };
    const auto result{ longer.inverse(target, seed) };
    ASSERT_TRUE(result);
    const auto pose{ longer.forward(*result) };
    ASSERT_TRUE(pose);
    expectPoint(pose->position, target);
}

TEST(ScaraKinematics, SingularEndpointsAreRejectedButForwardStateRemainsAvailable)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition seed{ 0_deg, 60_deg, 0_mm };
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
    const Kinematics::JointPosition singular{ 30_deg, -45_deg, 0_mm };
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
    config.base_origin = { -50_mm, 40_mm, 100_mm };
    config.base_yaw = 23_deg;
    config.tool_offset = { 35_mm, 20_mm, -10_mm };
    config.tool_yaw = 17_deg;
    const Kinematics kinematics{ config };
    for (const auto shoulder : { -720_deg, -179_deg, -90_deg, 0_deg, 90_deg, 179_deg, 720_deg }) {
        for (const auto elbow : { -135_deg, -70_deg, -20_deg, 20_deg, 70_deg, 135_deg }) {
            const Kinematics::JointPosition joints{ shoulder, elbow, 45_mm };
            const auto pose{ kinematics.forward(joints) };
            ASSERT_TRUE(pose);
            const auto result{ kinematics.inverse(pose->position, { shoulder + 2_deg, elbow, 40_mm }) };
            ASSERT_TRUE(result);
            expectJoints(*result, joints);
        }
    }
}

TEST(ScaraKinematics, VelocityJacobianMatchesAnalyticExampleAndItsInverse)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition joints{ 0_deg, 90_deg, 10_mm };
    const Kinematics::JointVelocity rates{ 1_rad_s, 2_rad_s, -5_mm_s };
    const auto velocity{ kinematics.forwardVelocity(joints, rates) };
    ASSERT_TRUE(velocity);
    expectNear(velocity->linear.x, -300_mm_s);
    expectNear(velocity->linear.y, 200_mm_s);
    expectNear(velocity->linear.z, -5_mm_s);
    expectNear(velocity->yaw, 3_rad_s);
    const auto inverse{ kinematics.inverseVelocity(joints, velocity->linear) };
    ASSERT_TRUE(inverse);
    expectNear(inverse->shoulder, rates.shoulder);
    expectNear(inverse->elbow, rates.elbow);
    expectNear(inverse->z, rates.z);
}

TEST(ScaraKinematics, VelocityMatchesFiniteDifferenceOfForwardPositionWithToolAndBaseOffsets)
{
    auto config{ BASIC };
    config.base_origin = { 20_mm, 30_mm, 40_mm };
    config.base_yaw = -47_deg;
    config.tool_offset = { 45_mm, -30_mm, -20_mm };
    const Kinematics kinematics{ config };
    const Kinematics::JointVelocity velocity{ 12_rpm, -8_rpm, 7_mm_s };
    const auto dt{ Time::create(1e-5) };
    for (const auto elbow : { -130_deg, -40_deg, 30_deg, 120_deg }) {
        const Kinematics::JointPosition joints{ 400_deg, elbow, 10_mm };
        const auto before{ kinematics.forward({ joints.shoulder - velocity.shoulder * dt,
                                                joints.elbow - velocity.elbow * dt,
                                                joints.z - velocity.z * dt }) };
        const auto after{ kinematics.forward({ joints.shoulder + velocity.shoulder * dt,
                                               joints.elbow + velocity.elbow * dt,
                                               joints.z + velocity.z * dt }) };
        const auto derivative{ kinematics.forwardVelocity(joints, velocity) };
        ASSERT_TRUE(before);
        ASSERT_TRUE(after);
        ASSERT_TRUE(derivative);
        expectNear(derivative->linear.x, (after->position.x - before->position.x) / (2.0 * dt), 1e-8);
        expectNear(derivative->linear.y, (after->position.y - before->position.y) / (2.0 * dt), 1e-8);
        expectNear(derivative->linear.z, (after->position.z - before->position.z) / (2.0 * dt), 1e-8);
        expectNear(derivative->yaw, (after->yaw - before->yaw) / (2.0 * dt), 1e-8);
        const auto inverse{ kinematics.inverseVelocity(joints, derivative->linear) };
        ASSERT_TRUE(inverse);
        expectNear(inverse->shoulder, velocity.shoulder);
        expectNear(inverse->elbow, velocity.elbow);
        expectNear(inverse->z, velocity.z);
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
    const Kinematics::JointPosition joints{ shoulder.toAxisPosition(-15_deg),
                                            elbow.toAxisPosition(315_deg),
                                            vertical.toAxisPosition(315_deg) };
    const Kinematics kinematics{ BASIC };
    const auto pose{ kinematics.forward(joints) };
    ASSERT_TRUE(pose);
    expectPoint(pose->position, { 173.20508075688772_mm, 200_mm, 10_mm });
    const auto inverse{ kinematics.inverse(pose->position, joints) };
    ASSERT_TRUE(inverse);
    expectNear(shoulder.toMotorPosition(inverse->shoulder), -15_deg);
    expectNear(elbow.toMotorPosition(inverse->elbow), 315_deg);
    expectNear(vertical.toMotorPosition(inverse->z), 315_deg);
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
    config.limits = Kinematics::JointLimits{ { 90_deg, 0_deg, 0_mm }, { 0_deg, 90_deg, 10_mm } };
    EXPECT_THROW(Kinematics{ config }, std::invalid_argument);
}

TEST(ScaraKinematics, NonfiniteInputsInvalidBranchesAndOverflowAreReported)
{
    const Kinematics kinematics{ BASIC };
    const Kinematics::JointPosition seed{ 0_deg, 90_deg, 0_mm };
    const Kinematics::CartesianPosition target{ 200_mm, 100_mm, 0_mm };
    for (const auto value :
         { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        const Kinematics::JointPosition bad{ Angle::create(value), 90_deg, 0_mm };
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
