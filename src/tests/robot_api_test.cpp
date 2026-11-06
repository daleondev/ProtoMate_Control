#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/examples/ContextThread.hpp"
#include "common/icc/transport/InProcessTransport.hpp"
#include "runtime/thread.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/robot.hpp"
#include "ui/cli/motion.hpp"
#include "ui/motion/Robot.hpp"
#include <gtest/gtest.h>
#include <future>
#include <numbers>
#include <sstream>

namespace
{
    namespace tr = motion::trajectory;
    using Robot = ui::motion::Robot;
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;
    tr::Capabilities caps()
    {
        tr::Capabilities c;
        c.step_radians.fill(std::numbers::pi / 1600);
        c.reference_steps.fill(1200);
        c.limits.fill({ -1e8, 1e8, 99999, 1e6, 1e6, 1e7 });
        c.hardware_fingerprint = tr::fingerprint(c);
        return c;
    }
    Robot::PreviewRequest request()
    {
        return { .move = { .joints = { 10_deg, 80_deg, 40_mm } },
                 .model = Robot::Model::Fixture180,
                 .start = { 0_rad, 1.2_rad, 30_mm } };
    }
    template<typename P>
    bool waitUntil(P predicate)
    {
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() > until) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
    class RobotApi : public testing::Test
    {
      protected:
        pnm::msg::Bus io, ui;
        pnm::coro::Context ioContext, uiContext;
        decltype(icc::InProcessTransport::pair()) endpoints{ icc::InProcessTransport::pair() };
        std::jthread ioRunner{ example::run_context(ioContext) }, uiRunner{ example::run_context(uiContext) };
        icc::Bridge ioBridge{ io, ioContext, *endpoints[0] }, uiBridge{ ui, uiContext, *endpoints[1] };
        Robot robot{ ui };
        ui::cli::Parser parser;
        std::atomic_uint inspected{}, mutated{};
        pnm::msg::ServiceServer<tr::Packet, tr::Packet> service{
            io.service<^^icc::Services::io_trajectory>().serve([this](const tr::Packet& packet) {
            const auto r = tr::decodeRequest(packet);
            tr::Reply reply;
            reply.capabilities = caps();
            if (r && r->operation == tr::Operation::Inspect) {
                ++inspected;
            }
            else {
                ++mutated;
                reply.error = tr::Error::Unsupported;
            }
            return tr::encode(reply);
        })
        };
        std::jthread server, planner;
        void SetUp() override
        {
            ioBridge.registerServices<icc::Services>(icc::ServiceProvider::IO);
            uiBridge.registerServices<icc::Services>(icc::ServiceProvider::UI);
            uiBridge.setConnectionCallback(
              [this](auto status) { robot.setConnected(status.state == icc::ConnectionState::Connected); });
            ui::cli::robot::setup(parser, robot);
            server = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    service.poll();
                    std::this_thread::sleep_for(1ms);
                }
            });
            ioBridge.start();
            uiBridge.start();
            ASSERT_TRUE(waitUntil(
              [this] { return uiBridge.connectionStatus().state == icc::ConnectionState::Connected; }));
        }
        void startPlanner()
        {
            planner = runtime::thread::create_jthread({ .name = "robot test planner", .stack_size = 65536U },
                                                      [this](std::stop_token stop) { robot.run(stop); });
        }
        void TearDown() override
        {
            planner.request_stop();
            if (planner.joinable()) {
                planner.join();
            }
            server.request_stop();
            server.join();
            ioBridge.stop();
            uiBridge.stop();
        }
        bool finished(std::uint64_t id)
        {
            const auto j = robot.job(id);
            return j && j->state != Robot::JobState::Planning && j->state != Robot::JobState::Queued;
        }
    };
}

TEST_F(RobotApi, PreviewTraversesSerializedIccAndNeverEnablesMotors)
{
    startPlanner();
    const auto id = robot.preview(request());
    ASSERT_TRUE(id);
    ASSERT_TRUE(waitUntil([&] { return finished(*id); }));
    const auto job = robot.job(*id);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->state, Robot::JobState::Preview) << tr::name(job->error);
    EXPECT_GT(job->plan.duration, 0);
    EXPECT_GT(job->plan.segments, 0U);
    EXPECT_EQ(inspected, 1U);
    EXPECT_EQ(mutated, 0U);
}
TEST_F(RobotApi, MissingPhysicalConfigurationCannotUseFixtureImplicitly)
{
    startPlanner();
    auto r = request();
    r.model = Robot::Model::Physical;
    const auto id = robot.preview(r);
    ASSERT_TRUE(id);
    ASSERT_TRUE(waitUntil([&] { return finished(*id); }));
    EXPECT_EQ(robot.job(*id)->error, tr::Error::Configuration);
    EXPECT_EQ(mutated, 0U);
}
TEST_F(RobotApi, BoundedQueueCancellationAndDisconnectHaveExplicitOutcomes)
{
    std::uint64_t first = 0;
    for (unsigned i = 0; i < tr::COMMANDS; ++i) {
        const auto id = robot.preview(request());
        ASSERT_TRUE(id);
        if (!i) {
            first = *id;
        }
    }
    EXPECT_EQ(robot.preview(request()).error(), tr::Error::Capacity);
    EXPECT_EQ(robot.cancel(first), tr::Error::None);
    EXPECT_EQ(robot.job(first)->state, Robot::JobState::Cancelled);
    ASSERT_TRUE(robot.preview(request()));
    robot.setConnected(false);
    for (auto id : robot.jobs()) {
        if (id) {
            EXPECT_EQ(robot.job(id)->state, Robot::JobState::Unknown);
        }
    }
    EXPECT_EQ(robot.preview(request()).error(), tr::Error::Unknown);
    EXPECT_EQ(mutated, 0U);
}
TEST_F(RobotApi, CliRequiresExplicitStartAndReportsSimulationUnits)
{
    startPlanner();
    std::ostringstream out;
    EXPECT_FALSE(parser.execute("robot preview joints 10 80 40 --fixture 180", out));
    out.str({});
    out.clear();
    ASSERT_TRUE(parser.execute(
      "robot preview joints 10 80 40 --fixture 180 --start-shoulder 0 --start-elbow 68.7549354 --start-z 30",
      out));
    ASSERT_TRUE(waitUntil([&] { return finished(1); }));
    out.str({});
    out.clear();
    ASSERT_TRUE(parser.execute("robot jobs 1", out));
    EXPECT_NE(out.str().find("preview ready"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("180 simulation"), std::string::npos);
    EXPECT_NE(out.str().find("steps/s^3"), std::string::npos);
    EXPECT_EQ(mutated, 0U);
}

TEST(RobotCancellation, ActivePreviewReleasesServiceAndLetsNextJobRun)
{
    pnm::msg::Bus bus;
    Robot robot{ bus };
    std::vector<pnm::msg::Reply<tr::Packet>> replies;
    auto service{ bus.service<^^icc::Services::io_trajectory>().serveDeferred(
      [&](const tr::Packet&, pnm::msg::Reply<tr::Packet> reply) { replies.push_back(std::move(reply)); }) };
    robot.setConnected(true);
    auto worker{ runtime::thread::create_jthread({ .name = "cancel planner", .stack_size = 65536U },
                                                 [&](std::stop_token stop) { robot.run(stop); }) };
    const auto first{ robot.preview(request()) };
    ASSERT_TRUE(first);
    ASSERT_TRUE(waitUntil([&] {
        service.poll();
        return replies.size() == 1;
    }));
    EXPECT_EQ(robot.cancel(*first), tr::Error::None);
    EXPECT_FALSE(replies[0].pending());
    const auto second{ robot.preview(request()) };
    ASSERT_TRUE(second);
    ASSERT_TRUE(waitUntil([&] {
        service.poll();
        return replies.size() == 2;
    }));
    EXPECT_EQ(robot.job(*first)->state, Robot::JobState::Cancelled);
    tr::Reply reply;
    reply.capabilities = caps();
    EXPECT_FALSE(replies[0].respond(tr::encode(reply)));
    ASSERT_TRUE(replies[1].respond(tr::encode(reply)));
    ASSERT_TRUE(waitUntil([&] { return robot.job(*second)->state == Robot::JobState::Preview; }));
}

TEST(RobotCancellation, DisconnectCancelsActiveRequestWithoutReplayOnReconnect)
{
    pnm::msg::Bus bus;
    Robot robot{ bus };
    std::optional<pnm::msg::Reply<tr::Packet>> reply;
    auto service{ bus.service<^^icc::Services::io_trajectory>().serveDeferred(
      [&](const tr::Packet&, pnm::msg::Reply<tr::Packet> value) { reply.emplace(std::move(value)); }) };
    robot.setConnected(true);
    auto worker{ runtime::thread::create_jthread({ .name = "disconnect planner", .stack_size = 65536U },
                                                 [&](std::stop_token stop) { robot.run(stop); }) };
    const auto job{ robot.preview(request()) };
    ASSERT_TRUE(job);
    ASSERT_TRUE(waitUntil([&] {
        service.poll();
        return reply.has_value();
    }));
    robot.setConnected(false);
    EXPECT_FALSE(reply->pending());
    robot.setConnected(true);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(service.poll(), 0U);
    EXPECT_EQ(robot.job(*job)->state, Robot::JobState::Unknown);
}

TEST(MotionCancellation, RobotStopCancelsPreparationWithoutWaitingForTelemetry)
{
    pnm::msg::Bus bus;
    ui::motion::Client client{ bus };
    Robot robot{ bus, client };
    client.setConnected(true);
    robot.setConnected(true);
    const auto robotJob = robot.home();
    const auto axisJob = client.create(motion::Family::Axis, 1);
    ui::cli::Parser parser;
    ui::cli::motion::setup(parser, client, robot, {});
    std::ostringstream output;
    EXPECT_TRUE(parser.execute("robot stop", output)) << output.str();
    EXPECT_TRUE(client.cancellation(robotJob).stop_requested());
    EXPECT_FALSE(client.cancellation(axisJob).stop_requested());
    const auto jobs = client.jobs();
    ASSERT_EQ(jobs.size(), 2U);
    EXPECT_EQ(jobs[0].state, ui::motion::Client::JobState::Stopping);
    EXPECT_EQ(jobs[1].state, ui::motion::Client::JobState::Submitting);
}

TEST(MotionCancellation, QueuedAcceptanceCannotUndoAnAlreadyRequestedStop)
{
    using Client = ui::motion::Client;
    using Execution = pnm::msg::ActionExecution<motion::MotorProgress, motion::MotorResult>;
    pnm::msg::Bus bus;
    Client client{ bus };
    client.setConnected(true);
    auto commands = bus.service<^^icc::Services::io_motor>().serve(
      [](const motion::MotorRequest&) { return motion::MotorResponse{ .message = {}, .commandGeneration = 1 }; });
    auto initialized = std::async(std::launch::async, [&] {
        return client.request({ .operation = motion::MotorOperation::Initialize });
    });
    ASSERT_TRUE(waitUntil([&] { return commands.poll() != 0; }));
    ASSERT_TRUE(initialized.get().ok);
    std::optional<Execution> execution;
    auto actions = bus.action<^^icc::Actions::io_motor_motion>().serveDeferred(
      [&](const motion::MotorGoal&, Execution value) { execution.emplace(std::move(value)); });
    const auto job = client.submit({ .position = 1, .velocity = 5 });
    ASSERT_EQ(actions.poll(), 1U);
    ASSERT_TRUE(execution);
    ASSERT_TRUE(execution->accept());
    ASSERT_TRUE(execution->feedback({ .position = 123 }));
    client.cancel(job);
    // Acceptance is already queued, but callbacks have not been dispatched yet.
    std::jthread worker([&](std::stop_token stop) { client.run(stop); });
    ASSERT_TRUE(waitUntil([&] { return client.jobs().front().progress.position == 123; }));
    EXPECT_EQ(client.jobs().front().state, Client::JobState::Stopping);
    ASSERT_TRUE(execution->cancelled({ .outcome = motion::MotorOutcome::Stopped, .final_state = {} }));
    EXPECT_TRUE(waitUntil([&] { return client.jobs().front().state == Client::JobState::Stopped; }));
}

TEST(MotionAdmission, AcquireUsesTheKnownBootAndReturnsOneAuthoritativeSnapshot)
{
    pnm::msg::Bus bus;
    ui::motion::Client client{ bus };
    client.setConnected(true);
    std::vector<tr::Operation> operations;
    auto service = bus.service<^^icc::Services::io_trajectory>().serve([&](const tr::Packet& packet) {
        const auto request = tr::decodeRequest(packet);
        EXPECT_TRUE(request);
        tr::Reply reply{ .boot = 7 };
        if (request) {
            operations.push_back(request->operation);
            if (request->operation == tr::Operation::Acquire) {
                EXPECT_EQ(request->boot, 7U);
                reply.session = 42;
                reply.position = { 10, 20, 30 };
            }
        }
        return tr::encode(reply);
    });
    auto inspection = std::async(std::launch::async, [&] { return client.inspect(); });
    ASSERT_TRUE(waitUntil([&] { service.poll(); return inspection.wait_for(0ms) == std::future_status::ready; }));
    EXPECT_EQ(inspection.get().boot, 7U);
    const auto job = client.create(motion::Family::Axis, 1);
    auto admission = std::async(std::launch::async, [&] { return client.acquire(job, 123); });
    ASSERT_TRUE(waitUntil([&] { service.poll(); return admission.wait_for(0ms) == std::future_status::ready; }));
    const auto result = admission.get();
    EXPECT_EQ(result.session, 42U);
    EXPECT_EQ(result.position, (tr::Counts{ 10, 20, 30 }));
    EXPECT_EQ(operations, (std::vector{ tr::Operation::Inspect, tr::Operation::Acquire }));
}

TEST(MotionLease, ValidatesAcknowledgementsWithoutClaimingThatFailedRenewalsStoppedMotion)
{
    enum class Response { Corrupt, WrongBoot, WrongVersion, FullSnapshot, Rejected, ServiceFailure, Valid };
    pnm::msg::Bus bus;
    ui::motion::Client client{ bus };
    client.setConnected(true);
    std::atomic response{ Response::Corrupt };
    auto trajectory = bus.service<^^icc::Services::io_trajectory>().serve([](const tr::Packet& packet) {
        const auto request = tr::decodeRequest(packet);
        EXPECT_TRUE(request);
        tr::Reply reply{ .boot = 17, .session = 5 };
        return tr::encode(reply);
    });
    auto control = bus.service<^^icc::Services::io_trajectory_control>().serve([&](const tr::Packet& packet) {
        const auto request = tr::decodeRequest(packet);
        EXPECT_TRUE(request);
        EXPECT_EQ(request->operation, tr::Operation::Renew);
        EXPECT_EQ(request->boot, 17U);
        tr::Reply reply{ .boot = 17, .acknowledgement = true };
        const auto selected = response.load();
        if (selected == Response::ServiceFailure) throw std::runtime_error("test service failure");
        if (selected == Response::WrongBoot) reply.boot = 18;
        if (selected == Response::Rejected) reply.error = tr::Error::LeaseExpired;
        if (selected == Response::FullSnapshot) reply.acknowledgement = false;
        auto encoded = tr::encode(reply);
        if (selected == Response::Corrupt) encoded.bytes[4] ^= std::byte{1};
        if (selected == Response::WrongVersion) encoded.bytes[1] ^= std::byte{1};
        return encoded;
    });
    std::jthread server([&](std::stop_token stop) {
        while(!stop.stop_requested()) {
            trajectory.poll();
            control.poll();
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto job = client.create(motion::Family::Axis, 1);
    ASSERT_EQ(client.acquire(job, 123).error, tr::Error::None);
    std::jthread worker([&](std::stop_token stop) { client.run(stop); });
    const std::array cases{
      std::pair{Response::Corrupt, tr::Error::Checksum},
      std::pair{Response::WrongBoot, tr::Error::Stale},
      std::pair{Response::WrongVersion, tr::Error::Protocol},
      std::pair{Response::FullSnapshot, tr::Error::Protocol},
      std::pair{Response::Rejected, tr::Error::LeaseExpired},
      std::pair{Response::ServiceFailure, tr::Error::Unknown}};
    for(const auto& [reply, error]:cases) {
        const auto failures=client.leaseStatus().failures;
        response=reply;
        ASSERT_TRUE(waitUntil([&] {
            const auto status=client.leaseStatus();
            return status.failures>failures && status.error==error;
        }));
        EXPECT_EQ(client.jobs().front().state, ui::motion::Client::JobState::Planning);
        EXPECT_EQ(client.leaseStatus().acknowledgements, 0U);
    }
    EXPECT_TRUE(client.leaseStatus().transport_error);
    response=Response::Valid;
    ASSERT_TRUE(waitUntil([&] {return client.leaseStatus().acknowledgements>0;}));
    const auto status=client.leaseStatus();
    EXPECT_EQ(status.error, tr::Error::None);
    EXPECT_FALSE(status.transport_error);
    EXPECT_TRUE(status.last_acknowledgement);
    EXPECT_EQ(client.jobs().front().state, ui::motion::Client::JobState::Planning);
}
