#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/examples/ContextThread.hpp"
#include "common/icc/transport/InProcessTransport.hpp"
#include "runtime/thread.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/robot.hpp"
#include "ui/motion/Robot.hpp"
#include <gtest/gtest.h>
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
