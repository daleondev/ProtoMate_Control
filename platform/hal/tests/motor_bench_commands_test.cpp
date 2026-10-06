#include "motor_bench/Commands.hpp"

#include <gtest/gtest.h>
#include <string>
#include <utility>

using motor_bench::Operation;
using motor_bench::parse;

TEST(MotorBenchCommands, DefaultMoveSelectsOnlyTheRequestedAxes)
{
    for (const auto& [line, axes] : {
             std::pair{ "move m1", 1U }, { "move m2", 2U }, { "move both", 3U } }) {
        SCOPED_TRACE(line);
        const auto command = parse(line);
        ASSERT_TRUE(command);
        EXPECT_EQ(command->operation, Operation::Move);
        EXPECT_EQ(command->axes, axes);
        EXPECT_EQ(command->pulses, 400U);
        EXPECT_EQ(command->hertz, 200U);
    }
}

TEST(MotorBenchCommands, ExplicitDirectionAndOptionalTiming)
{
    const auto forward = parse("  forward\tm1 1 50\r\n");
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->operation, Operation::Forward);
    EXPECT_EQ(forward->axes, 1U);
    EXPECT_EQ(forward->pulses, 1U);
    EXPECT_EQ(forward->hertz, 50U);
    const auto reverse = parse("reverse m2 800");
    ASSERT_TRUE(reverse);
    EXPECT_EQ(reverse->operation, Operation::Reverse);
    EXPECT_EQ(reverse->axes, 2U);
    EXPECT_EQ(reverse->pulses, 800U);
    EXPECT_EQ(reverse->hertz, 200U);
}

TEST(MotorBenchCommands, PulseFrequencyAndDurationLimitsAreInclusive)
{
    for (auto line : { "move both 3200 1000", "forward m1 1000 50", "reverse m2 3200 160" }) {
        SCOPED_TRACE(line);
        EXPECT_TRUE(parse(line));
    }
    for (auto line : { "move m1 0", "move m1 3201", "move m1 1 49", "move m1 1 1001",
                       "move m1 1001 50", "move m1 3200 159" }) {
        SCOPED_TRACE(line);
        EXPECT_FALSE(parse(line));
    }
}

TEST(MotorBenchCommands, RejectsMalformedNumbersWithoutPartialExecution)
{
    for (auto line : { "move m1 -1", "move m1 +1", "move m1 1.5", "move m1 1foo",
                       "move m1 0x10", "move m1 4294967296", "move m1 1 4294967296",
                       "move m1 1 100x", "move m1 1 -100", "move m1 nan", "move m1 inf" }) {
        SCOPED_TRACE(line);
        EXPECT_FALSE(parse(line));
    }
}

TEST(MotorBenchCommands, FeedbackAndControlCommandsHaveNoHiddenMotionArguments)
{
    for (const auto& [line, operation] : {
             std::pair{ "help", Operation::Help }, { "check", Operation::Check },
             { "status", Operation::Status }, { "hold", Operation::Hold },
             { "stop", Operation::Stop }, { "independent", Operation::Independent } }) {
        SCOPED_TRACE(line);
        const auto command = parse(line);
        ASSERT_TRUE(command);
        EXPECT_EQ(command->operation, operation);
        EXPECT_EQ(command->axes, 0U);
        EXPECT_FALSE(parse(std::string{ line } + " m1"));
    }
    for (const auto& [line, axes] : {
             std::pair{ "feedback m1", 1U }, { "feedback m2", 2U }, { "feedback both", 3U } }) {
        SCOPED_TRACE(line);
        const auto command = parse(line);
        ASSERT_TRUE(command);
        EXPECT_EQ(command->operation, Operation::Feedback);
        EXPECT_EQ(command->axes, axes);
        EXPECT_FALSE(parse(std::string{ line } + " 100"));
    }
}

TEST(MotorBenchCommands, MissingAxesExtraWordsAndUnknownCommandsAreRejected)
{
    for (auto line : { "move", "forward", "reverse", "feedback", "move m3", "feedback m3",
                       "move both 400 200 extra", "move both 400 200 extra more", "hold m1",
                       "move m1; stop", "move m1\nstop", "mode spread", "mo m1" }) {
        SCOPED_TRACE(line);
        EXPECT_FALSE(parse(line));
    }
    for (auto line : { "", " \t\r\n" }) {
        const auto command = parse(line);
        ASSERT_TRUE(command);
        EXPECT_EQ(command->operation, Operation::Help);
        EXPECT_EQ(command->axes, 0U);
    }
}
