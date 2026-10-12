#include "control/Messages.hpp"
#include "p2p/bridge/Frame.hpp"

#include <gtest/gtest.h>

namespace
{
    namespace msg = control::messages;
    namespace memory = pnm::utils::memory;

    template<memory::Serializable T>
    T roundTrip(const T& value)
    {
        auto bytes = memory::serialize(value);
        T decoded{};
        if (!memory::deserialize(bytes, decoded))
            throw std::runtime_error("message round trip failed");
        return decoded;
    }

    TEST(ControlMessages, ConfigurationAndGoalsUsePneumoSerialization)
    {
        msg::Request request{};
        request.operation = msg::Operation::ConfigureAxis;
        request.motor = 2;
        request.conversion = LinearAxisConversion({ .travel_per_output_revolution = 40_mm });
        const auto decoded = roundTrip(request);
        ASSERT_EQ(decoded.operation, request.operation);
        ASSERT_EQ(decoded.motor, request.motor);
        ASSERT_TRUE(decoded.conversion);
        EXPECT_EQ(
          std::get<LinearAxisConversion>(*decoded.conversion).configuration().travel_per_output_revolution,
          40_mm);

        msg::Goal goal{};
        goal.operation = msg::Operation::Trajectory;
        goal.trajectory.profile.size = motion::Profile::MAX_PHASES;
        goal.trajectory.configuration_revision = 13;
        goal.trajectory.command_revision = 17;
        goal.trajectory.pulses = { 100, 200, 300 };
        goal.trajectory.forward = { true, false, true };
        const auto copy = roundTrip(goal);
        EXPECT_EQ(copy.trajectory.configuration_revision, 13);
        EXPECT_EQ(copy.trajectory.command_revision, 17);
        EXPECT_EQ(copy.trajectory.pulses, goal.trajectory.pulses);
        EXPECT_EQ(copy.trajectory.profile.size, motion::Profile::MAX_PHASES);
        EXPECT_EQ(copy.trajectory.forward, goal.trajectory.forward);
    }

    TEST(ControlMessages, SnapshotsPreserveValuesAndReconstructLocalErrors)
    {
        control::ControllerTypes::PlanningSnapshot snapshot{};
        snapshot.status.enabled = true;
        snapshot.status.command_revision = 123;
        snapshot.status.axes[0].actual_position = 42_deg;
        snapshot.status.axes[1].actual_velocity = std::unexpected(std::make_error_code(std::errc::io_error));
        msg::Response response{};
        response.state = msg::State(snapshot);
        response.drivers[0].name = "DM542T";
        response.drivers[1].name = "TMC2209";
        response.drivers[1].state.error = std::errc::timed_out;
        const auto copy = roundTrip(response);
        const auto state = copy.state.snapshot();
        EXPECT_TRUE(state.status.enabled);
        EXPECT_EQ(state.status.command_revision, 123);
        ASSERT_TRUE(state.status.axes[0].actual_position);
        EXPECT_EQ(*state.status.axes[0].actual_position, 42_deg);
        ASSERT_FALSE(state.status.axes[1].actual_velocity);
        EXPECT_EQ(state.status.axes[1].actual_velocity.error(), std::make_error_code(std::errc::io_error));
        EXPECT_EQ(copy.drivers[0].name, "DM542T");
        EXPECT_EQ(copy.drivers[1].status().status.error, std::make_error_code(std::errc::timed_out));
    }

    TEST(ControlMessages, ActionResultsAndFaultsUseAutomaticAdapters)
    {
        const msg::Outcome outcome{ .result = control::MotorResult::Rejected,
                                    .error = "stale plan",
                                    .invalid_argument = true };
        const auto copy = roundTrip(outcome);
        EXPECT_EQ(copy.result, outcome.result);
        EXPECT_EQ(copy.error, outcome.error);
        EXPECT_TRUE(copy.invalid_argument);
        msg::FaultState faults{};
        faults.link_inhibited = true;
        faults.drivers[1].fault_latched = true;
        faults.drivers[1].error = std::errc::io_error;
        const auto received = roundTrip(faults);
        EXPECT_TRUE(received.link_inhibited);
        EXPECT_TRUE(received.drivers[1].fault_latched);
        EXPECT_EQ(received.drivers[1].status().error, std::make_error_code(std::errc::io_error));
    }

    TEST(ControlMessages, CompleteMessagesFitTransportFrames)
    {
        msg::Response response{};
        response.error.assign(msg::MAX_ERROR_TEXT, 'x');
        for (auto& driver : response.drivers)
            driver.name = "TMC2209";
        const auto reply = p2p::util::encode_frame(p2p::util::FrameKind::Response, {}, 1, response);
        EXPECT_LE(reply.size, p2p::util::MAX_FRAME_BYTES);
        const auto goal =
          p2p::util::encode_frame(p2p::util::FrameKind::ActionGoal, "motion.execute", 1, msg::Goal{});
        EXPECT_LE(goal.size, p2p::util::MAX_FRAME_BYTES);
        const auto state =
          p2p::util::encode_frame(p2p::util::FrameKind::Topic, "motion.state", 0, msg::State{});
        EXPECT_LE(state.size, p2p::util::MAX_FRAME_BYTES);
    }
}
