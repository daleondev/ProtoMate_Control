#include "common/motion/PulseCursor.hpp"
#include "io/motion/TrajectorySession.hpp"
#include <gtest/gtest.h>

namespace
{
    namespace tr = motion::trajectory;
    tr::Capabilities capabilities()
    {
        tr::Capabilities c;
        c.coordinated_execution = true;
        c.step_radians.fill(0.001);
        c.reference_steps.fill(0);
        c.limits.fill({ -1e8, 1e8, 100000, 1e9, 1e9, 1e12 });
        c.hardware_fingerprint = tr::fingerprint(c);
        return c;
    }
    tr::Segment line(double start, double end, std::uint64_t ticks = 100000)
    {
        tr::Segment s;
        s.ticks = ticks;
        s.motors[0] = { { start, end - start } };
        s.path_end = 1;
        return s;
    }
    std::vector<tr::Pulse> collect(tr::PulseCursor& c)
    {
        std::vector<tr::Pulse> pulses;
        for (;;) {
            const auto p = c.next();
            EXPECT_TRUE(p.has_value());
            if (!p || !*p) {
                break;
            }
            pulses.push_back(**p);
            if (pulses.size() > 10000) {
                ADD_FAILURE();
                break;
            }
        }
        return pulses;
    }
    class Executor final : public io::motion::ITrajectoryExecutor
    {
      public:
        tr::Reply status;
        tr::Error commitError{ tr::Error::None };
        std::shared_ptr<const io::motion::TrajectoryWindow> active;
        unsigned commits{};
        Executor()
        {
            status.capabilities = capabilities();
            status.state = tr::State::Ready;
            status.enabled = true;
            status.referenced = 7;
            status.generation = 3;
        }
        tr::Reply snapshot() override { return status; }
        tr::Error acquire(std::uint64_t s) override
        {
            if (status.session) {
                return tr::Error::Busy;
            }
            status.session = s;
            return tr::Error::None;
        }
        tr::Error release(std::uint64_t) override
        {
            status.session = 0;
            return tr::Error::None;
        }
        tr::Error commit(std::shared_ptr<const io::motion::TrajectoryWindow> w, const tr::Request&) override
        {
            if (commitError != tr::Error::None) {
                return commitError;
            }
            active = std::move(w);
            ++commits;
            return tr::Error::None;
        }
        tr::Error control(tr::Operation o) override
        {
            if (o == tr::Operation::Abort || o == tr::Operation::Disable) {
                active.reset();
                status.session = 0;
                status.referenced = 0;
                ++status.generation;
            }
            return tr::Error::None;
        }
    };
    class RobotSession : public testing::Test
    {
      protected:
        Executor executor;
        io::motion::TrajectorySession session{ executor, 123 };
        tr::Request request;
        void SetUp() override
        {
            request.operation = tr::Operation::Acquire;
            request.boot = 123;
            request.generation = 3;
            request.configuration = 456;
            const auto r = session.request(request);
            ASSERT_EQ(r.error, tr::Error::None);
            request.session = r.session;
            request.revision = 1;
        }
        tr::Request upload(bool rest = true)
        {
            auto r = request;
            r.operation = tr::Operation::Upload;
            r.count = 1;
            r.final = true;
            r.segments[0].ticks = 100000;
            r.segments[0].motors[0] = motion::hermite({}, { 1, rest ? 0.0 : 100.0, 0 }, 0.01);
            r.segments[0].path_end = 1;
            return r;
        }
    };
}

TEST(RobotPulseCursor, AbsoluteTimesAndSignedReversals)
{
    tr::PulseCursor c{ capabilities(), {} };
    auto s = line(0, 10);
    ASSERT_EQ(c.append(s), tr::Error::None);
    const auto p = collect(c);
    ASSERT_EQ(p.size(), 10U);
    EXPECT_EQ(p.front().tick, 5000U);
    EXPECT_EQ(p.back().tick, 95000U);
    EXPECT_EQ(c.position()[0], 10);
    // Reverse with a C2 polynomial after a stationary endpoint.
    tr::PulseCursor reversed{ capabilities(), { 10, 0, 0 } };
    s.motors[0] = motion::hermite({ 10, 0, 0 }, { -2, 0, 0 }, 0.01);
    ASSERT_EQ(reversed.append(s), tr::Error::None);
    const auto negative = collect(reversed);
    ASSERT_EQ(negative.size(), 12U);
    EXPECT_FALSE(negative.front().forward);
    EXPECT_EQ(reversed.position()[0], -2);
}
TEST(RobotPulseCursor, CarryFractionalPositionWithoutSegmentDrift)
{
    tr::PulseCursor c{ capabilities(), {} };
    std::vector<tr::Pulse> pulses;
    for (unsigned i = 0; i < 100; ++i) {
        ASSERT_EQ(c.append(line(i * 0.1, (i + 1) * 0.1, 10000)), tr::Error::None);
        const auto next = collect(c);
        pulses.insert(pulses.end(), next.begin(), next.end());
    }
    ASSERT_EQ(pulses.size(), 10U);
    EXPECT_EQ(c.position()[0], 10);
    EXPECT_EQ(c.endTick(), 1000000U);
    for (unsigned i = 0; i < 10; ++i) {
        EXPECT_NEAR(pulses[i].tick, 50000 + i * 100000, 1);
    }
}
TEST(RobotPulseCursor, ReversesWithinOnePolynomialAndMergesStationaryAxes)
{
    auto s = line(0, 0);
    s.motors[0] = { { 0, 40, -40 } };
    s.motors[1] = { { 0, -3 } };
    tr::PulseCursor c{ capabilities(), {} };
    ASSERT_EQ(c.append(s), tr::Error::None);
    const auto p = collect(c);
    ASSERT_EQ(p.size(), 23U);
    EXPECT_EQ(c.position(), (tr::Counts{ 0, -3, 0 }));
    for (std::size_t i = 1; i < p.size(); ++i) {
        EXPECT_LE(p[i - 1].tick, p[i].tick);
    }
}
TEST(RobotPulseCursor, RejectsInfeasibleEdgesWithoutStretchingPeriods)
{
    auto caps = capabilities();
    caps.minimum_period_ticks = 1000;
    tr::PulseCursor c{ caps, {} };
    ASSERT_EQ(c.append(line(0, 1000, 10000)), tr::Error::VelocityLimit);
    caps.limits[0].velocity = 1e8;
    tr::PulseCursor d{ caps, {} };
    // First pulse is earlier than direction setup: no attempted timing repair.
    EXPECT_EQ(d.append(line(0, 1000, 10000)), tr::Error::Timing);
}
TEST(RobotPulseCursor, ExtendedTimelineAndZeroPulseSegments)
{
    const std::uint64_t origin = 0xffffffffULL - 1000;
    tr::PulseCursor c{ capabilities(), {}, origin };
    auto s = line(0, 0);
    ASSERT_EQ(c.append(s), tr::Error::None);
    EXPECT_TRUE(collect(c).empty());
    EXPECT_EQ(c.endTick(), origin + 100000);
    s.motors[0] = motion::hermite({}, { 2, 0, 0 }, 0.01);
    ASSERT_EQ(c.append(s), tr::Error::None);
    const auto p = collect(c);
    ASSERT_EQ(p.size(), 2U);
    EXPECT_GT(p.front().tick, 0xffffffffULL);
}
TEST_F(RobotSession, UploadNeverExecutesAndCommitPublishesOnce)
{
    auto r = upload();
    EXPECT_EQ(session.request(r).error, tr::Error::None);
    EXPECT_EQ(executor.commits, 0U);
    r.operation = tr::Operation::Commit;
    EXPECT_EQ(session.request(r).error, tr::Error::None);
    EXPECT_EQ(executor.commits, 1U);
    EXPECT_EQ(session.request(r).error, tr::Error::Stale);
    EXPECT_EQ(executor.commits, 1U);
}
TEST_F(RobotSession, IdenticalDuplicatesAcknowledgeConflictingDuplicatesReject)
{
    auto r = upload();
    auto a = session.request(r);
    ASSERT_EQ(a.error, tr::Error::None);
    auto b = session.request(r);
    EXPECT_EQ(b.error, tr::Error::None);
    EXPECT_EQ(a.credits, b.credits);
    r.segments[0].job = 42;
    EXPECT_EQ(session.request(r).error, tr::Error::Order);
}
TEST_F(RobotSession, InvalidChunkIsAtomicAndInterruptedUploadCannotRun)
{
    auto r = upload();
    r.count = 2;
    r.segments[1] = r.segments[0];
    EXPECT_EQ(session.request(r).error, tr::Error::Discontinuity);
    EXPECT_EQ(executor.commits, 0U);
    r.count = 1;
    EXPECT_EQ(session.request(r).error, tr::Error::None);
    request.operation = tr::Operation::Inspect;
    EXPECT_EQ(session.request(request).next_index, 1U);
    request.operation = tr::Operation::Abort;
    EXPECT_EQ(session.request(request).error, tr::Error::None);
    EXPECT_EQ(session.request(r).error, tr::Error::Stale);
    EXPECT_EQ(executor.commits, 0U);
}
TEST_F(RobotSession, RequiresCompleteStoppingPathAndStableCommitCursor)
{
    auto r = upload(false);
    ASSERT_EQ(session.request(r).error, tr::Error::None);
    request.operation = tr::Operation::Commit;
    EXPECT_EQ(session.request(request).error, tr::Error::NoStoppingPath);
    r.branch = tr::Branch::Stopping;
    r.segments[0].motors[0] = motion::hermite({ 1, 100, 0 }, { 1.5, 0, 0 }, 0.01);
    ASSERT_EQ(session.request(r).error, tr::Error::None);
    executor.commitError = tr::Error::Stale;
    EXPECT_EQ(session.request(request).error, tr::Error::Stale);
    EXPECT_FALSE(executor.active);
    executor.commitError = tr::Error::None;
    EXPECT_EQ(session.request(request).error, tr::Error::None);
    ASSERT_TRUE(executor.active);
    EXPECT_EQ(executor.active->stop().size(), 1U);
}
TEST_F(RobotSession, BoundsCheckpointAndRejectsOldBootOrGeneration)
{
    auto r = upload();
    r.segments[0].ticks = tr::CHECKPOINT_TICKS + 1;
    EXPECT_EQ(session.request(r).error, tr::Error::Timing);
    r = upload();
    r.boot = 122;
    EXPECT_EQ(session.request(r).error, tr::Error::Stale);
    r.boot = 123;
    r.generation = 2;
    EXPECT_EQ(session.request(r).error, tr::Error::Stale);
}

#include "io/motion/Ownership.hpp"
TEST(MotionOwnership, FamiliesExcludeBeforePlanningAndBufferedRawJobsRetainOwnership)
{
    io::motion::Ownership ownership;
    const auto now = io::motion::Ownership::Clock::now();
    using F = motion::Family;
    using E = motion::trajectory::Error;
    EXPECT_EQ(ownership.acquire(1,F::Motor,1,1,now).error, E::None);
    EXPECT_EQ(ownership.acquire(2,F::Motor,1,1,now).error, E::None);
    EXPECT_EQ(ownership.acquire(3,F::Motor,2,1,now).error, E::None);
    EXPECT_EQ(ownership.acquire(4,F::Axis,4,1,now).error, E::Busy);
    EXPECT_EQ(ownership.acquire(4,F::Robot,7,1,now).error, E::Busy);
    ownership.release(1);
    EXPECT_EQ(ownership.owners()[0], 2U);
    ownership.release(2); ownership.release(3);
    EXPECT_EQ(ownership.acquire(4,F::Axis,1,1,now).error, E::None);
    EXPECT_EQ(ownership.acquire(5,F::Axis,2,1,now).error, E::None);
    EXPECT_EQ(ownership.acquire(6,F::Axis,4,1,now).error, E::None);
    const auto busy = ownership.acquire(7,F::Axis,2,1,now);
    EXPECT_EQ(busy.error, E::Busy); EXPECT_EQ(busy.busy_job, 5U);
}
TEST(MotionOwnership, ExpiredLeaseCannotBeRevivedAndStoppingKeepsItsReservation)
{
    io::motion::Ownership ownership;
    const auto now = io::motion::Ownership::Clock::now();
    ASSERT_EQ(ownership.acquire(1,motion::Family::Robot,7,1,now).error, motion::trajectory::Error::None);
    EXPECT_FALSE(ownership.renew(1, now + std::chrono::milliseconds(250)));
    ownership.find(1)->state = motion::JobState::Stopping;
    EXPECT_FALSE(ownership.renew(1, now + std::chrono::milliseconds(100)));
    EXPECT_EQ(ownership.acquire(2,motion::Family::Axis,1,1,now).error, motion::trajectory::Error::Busy);
    const auto revisions = ownership.revisions();
    ownership.release(1);
    for (unsigned i=0;i<3;++i) EXPECT_EQ(ownership.revisions()[i], revisions[i]+1);
    EXPECT_EQ(ownership.acquire(2,motion::Family::Axis,1,1,now).error, motion::trajectory::Error::None);
}
TEST(TrajectoryProtocol, CompactProgramRoundTripsInsideExistingPacket)
{
    namespace t = motion::trajectory;
    t::Request request;
    request.operation = t::Operation::Prepare;
    request.family = motion::Family::Robot; request.axes=7;
    request.job=42; request.boot=7; request.session=9;
    request.axis_revisions={8,9,10}; request.expected_position={12,13,14};
    request.program.displacement={4000,-2000,0};
    request.program.progress_limits={1,10,10,100};
    request.program.progress=motion::Profile::create(1,request.program.progress_limits)->description();
    const auto encoded=t::encode(request);
    ASSERT_LT(encoded.size,t::Packet::CAPACITY);
    const auto decoded=t::decodeRequest(encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->program.displacement,request.program.displacement);
    EXPECT_EQ(decoded->axis_revisions,request.axis_revisions);
    EXPECT_EQ(decoded->program.progress.size,request.program.progress.size);
    EXPECT_EQ(decoded->program.progress.distance,1.);
    EXPECT_EQ(decoded->job,42U);
}
TEST(TrajectoryProtocol, ExecutionFailuresRemainDistinctAcrossTheBridge)
{
    namespace t = motion::trajectory;
    for (unsigned code=unsigned(t::Error::ExecutionDeadline); code<=unsigned(t::Error::SpliceRejected); ++code) {
        t::Reply reply;
        reply.error = t::Error(code);
        const auto decoded=t::decodeReply(t::encode(reply));
        ASSERT_TRUE(decoded) << code;
        EXPECT_EQ(decoded->error, reply.error);
        EXPECT_NE(t::name(reply.error), "invalid error");
        EXPECT_NE(t::name(reply.error), t::name(t::Error::Timing));
    }
}
