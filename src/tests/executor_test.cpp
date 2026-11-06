#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/tests/fixtures/SimulatedStepHardware.hpp"
#include "io/motion/ProfileSchedule.hpp"
#include "../../examples/giga_motion_api_test/FixturePosition.hpp"
#include <gtest/gtest.h>

namespace
{
    using namespace std::chrono_literals;
    using Schedule = io::motion::ProfileSchedule;
    constexpr motion::Profile::Limits limits{ 25000, 250000, 250000, 2500000 };
    class ExecutorTest : public testing::Test
    {
      protected:
        hal::util::SimulatedStepHardware* hw{};
        std::shared_ptr<hal::util::StepGenerator> gen;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> out;
        std::shared_ptr<Schedule> nominal;
        std::function<void()> progress;
        void SetUp() override
        {
            auto model = std::make_unique<hal::util::SimulatedStepHardware>();
            hw = model.get();
            gen = std::make_shared<hal::util::StepGenerator>(std::move(model));
            hw->interrupt = [this] { gen->service(); };
            ASSERT_TRUE(gen->setProgressCallback([this](const auto&) noexcept {
                if (progress)
                    progress();
            }));
            nominal = std::make_shared<Schedule>(*motion::Profile::create(10000, limits));
            ASSERT_TRUE(nominal->valid());
            ASSERT_TRUE(gen->start());
            std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
            for (unsigned i = 0; i < 3; ++i) {
                out[i] = gen->output(hal::step::Axis(i));
                ASSERT_TRUE(out[i]->prepareSequence(nominal));
                delays[i] = 1ms + std::chrono::nanoseconds(nominal->firstTick() * 100ULL);
            }
            ASSERT_TRUE(gen->startPrepared(delays));
        }
        std::vector<std::uint64_t> rises(unsigned axis)
        {
            std::vector<std::uint64_t> result;
            for (auto e : hw->edges)
                if (e.axis == axis && e.high)
                    result.push_back(e.tick);
            return result;
        }
        void TearDown() override
        {
            progress = {};
            hw->sample_filter = {};
            gen->stop();
        }
    };
}
TEST(MotionFixture, CounterResetDoesNotMoveTheSyntheticReferenceSwitch)
{
    motion_fixture::Position position;
    position.observe(32, true);
    ASSERT_EQ(position.physical, 32);
    position.observe(0, false); // clear in Idle after forward homing.
    EXPECT_EQ(position.physical, 32);
    position.observe(400, false);
    EXPECT_EQ(position.physical, -368);
    position.observe(400, false); // A repeated status must not count twice.
    position.observe(0, true); // next forward seek.
    EXPECT_EQ(position.physical, -368);
    position.observe(400, true);
    EXPECT_EQ(position.physical, 32);
    position.observe(0, false);
    position.observe(1, false);
    EXPECT_EQ(position.physical, 31); // contact releases after one backoff step.
}
TEST(ProfileSchedule, RejectsEmptyWithoutInvalidNumericConversions)
{
    const Schedule empty(motion::Profile{});
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(empty.failure(), io::motion::ScheduleFailure::InvalidSource);
}
TEST(ProfileSchedule, ReportsDistinctStoppingConstructionFailures)
{
    const auto source = std::make_shared<Schedule::Nominal>(*motion::Profile::create(10000, limits));
    ASSERT_TRUE(source->valid);
    const Schedule beyond(source, source->end, limits);
    EXPECT_EQ(beyond.failure(), io::motion::ScheduleFailure::InvalidCut);
    auto invalidLimits = limits;
    invalidLimits.jerk = 0;
    const Schedule rejected(source, 600'000, invalidLimits);
    EXPECT_EQ(rejected.failure(), io::motion::ScheduleFailure::BrakeState);
    const motion::Profile empty;
    const Schedule duration(source, 600'000, limits, &empty);
    EXPECT_EQ(duration.failure(), io::motion::ScheduleFailure::BrakeDuration);
    EXPECT_EQ(Schedule(source, 600'000, limits).failure(), io::motion::ScheduleFailure::None);
}
TEST(ProfileBrake, ReplaysCapturedOneUlpAccelerationAtStoppingBoundary)
{
    // GIGA diagnostic: robot move joints -9 -9 -.3125 --speed 1,
    // renewal cut 1,420,551 on the 10 MHz timeline. The analytic phase state
    // is one representable double above its configured acceleration limit.
    const motion::Profile::Limits captured{1.7777777777768662, 7.1111111111068084,
                                         7.1111111111068084, 56.888888888849216};
    constexpr double velocity = .56572515555517222;
    constexpr double acceleration = 7.1111111111068093;
    ASSERT_EQ(acceleration, std::nextafter(captured.acceleration, INFINITY));
    const auto brake = motion::Profile::brake(velocity, acceleration, captured);
    ASSERT_TRUE(brake);
    EXPECT_DOUBLE_EQ(brake->at(0).velocity, velocity);
    EXPECT_DOUBLE_EQ(brake->at(0).acceleration, acceleration);
    EXPECT_LE(brake->distance(), .9728672097090203);
    EXPECT_NEAR(brake->at(brake->duration()).velocity, 0, 1e-12);
    EXPECT_NEAR(brake->at(brake->duration()).acceleration, 0, 1e-12);
}
TEST(ProfileBrake, RoundoffAllowanceIsRelativeAndDoesNotPermitRealLimitViolations)
{
    for (double scale : {1e-12, 1., 400., 1e12}) {
        const motion::Profile::Limits l{20*scale, 7*scale, 5*scale, 56*scale};
        for (double sign : {-1., 1.}) {
            const double bound = sign > 0 ? l.acceleration : l.deceleration;
            const double adjacent = sign * std::nextafter(bound, INFINITY);
            const auto brake = motion::Profile::brake(2*scale, adjacent, l);
            ASSERT_TRUE(brake) << scale << ", sign=" << sign;
            EXPECT_DOUBLE_EQ(brake->at(0).acceleration, adjacent);
            EXPECT_FALSE(motion::Profile::brake(2*scale, sign*bound*(1+1e-12), l));
            EXPECT_FALSE(motion::Profile::brake(2*scale, sign*bound*1.01, l));
        }
    }
    EXPECT_FALSE(motion::Profile::brake(1, NAN, limits));
    EXPECT_FALSE(motion::Profile::brake(1, INFINITY, limits));
    EXPECT_FALSE(motion::Profile::brake(-1, 0, limits));
}
TEST(ProfileSchedule, CapturedLimitsRemainValidAcrossPhaseJoinsAndAxisScaling)
{
    const motion::Profile::Limits l{1.7777777777768662, 7.1111111111068084,
                                  7.1111111111068084, 56.888888888849216};
    const auto law = motion::Profile::create(1., l);
    ASSERT_TRUE(law);
    std::vector<std::uint64_t> cuts{1'420'550, 1'420'551, 1'420'552};
    double time{};
    const auto description = law->description();
    for (const auto& phase : std::span(description.phases).first(description.size)) {
        time += phase.duration;
        const auto tick = std::uint64_t(std::llround(time*1e7));
        for (int delta=-1; delta<=1; ++delta) {
            const auto cut = tick+delta;
            if (cut && cut < std::ceil(law->duration()*1e7)) cuts.push_back(cut);
        }
    }
    for (const double scale : {1., 400., 1600., 6250.}) {
        const auto scaled = law->scaled(scale);
        ASSERT_TRUE(scaled);
        const auto source = std::make_shared<Schedule::Nominal>(*scaled);
        ASSERT_TRUE(source->valid);
        for (const auto cut : cuts) {
            const auto state = law->at(cut/1e7);
            const auto brake = motion::Profile::brake(state.velocity, state.acceleration, l);
            const bool nominalTail = law->brakingToRest(cut/1e7);
            ASSERT_TRUE(nominalTail || brake) << cut;
            const auto axisBrake = !nominalTail ? brake->scaled(scale) : std::nullopt;
            ASSERT_TRUE(nominalTail || axisBrake) << cut;
            const Schedule schedule(source, cut,
              {l.velocity*scale,l.acceleration*scale,l.deceleration*scale,l.jerk*scale},
              axisBrake ? &*axisBrake : nullptr);
            EXPECT_TRUE(schedule.valid()) << cut << ", scale=" << scale
                                          << ", failure=" << unsigned(schedule.failure());
        }
    }
}
TEST(ProfileSchedule, BanksShareAndRetainTheImmutableNominal)
{
    auto source = std::make_shared<Schedule::Nominal>(*motion::Profile::create(10000, limits));
    std::weak_ptr<const Schedule::Nominal> lifetime = source;
    auto a = std::make_unique<Schedule>(source, 600'000, limits);
    auto b = std::make_unique<Schedule>(source, 800'000, limits);
    source.reset();
    ASSERT_FALSE(lifetime.expired());
    ASSERT_TRUE(a->valid());
    ASSERT_TRUE(b->valid());
    ASSERT_EQ(a->commonPrefix(*b), 600'000 - std::int64_t(a->firstTick()));
    a.reset();
    ASSERT_TRUE(b->timing(0));
    b.reset();
    EXPECT_TRUE(lifetime.expired());
    // Fits the measured production M4 heap; increasing these needs a new budget.
    static_assert(sizeof(Schedule) <= 2800);
    static_assert(sizeof(Schedule::Nominal) <= 2800);
}
TEST_F(ExecutorTest, NominalHasExactCountAndAbsoluteEdgeTimes)
{
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        const auto edges = rises(i);
        ASSERT_EQ(edges.size(), 10000U);
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *nominal->risingTick(p));
        EXPECT_FALSE(hw->high[i]);
    }
}
TEST_F(ExecutorTest, SpliceDuringSlowStartReplacesAlreadyLoadedCompare)
{
    // No edge has yet occurred: the original first compare is in the future.
    hw->advance(10'000);
    auto brake = std::make_shared<Schedule>(*nominal, 100'000, limits);
    ASSERT_TRUE(brake->valid());
    std::array<std::shared_ptr<const hal::step::TimedSequence>, 3> tails{ brake, brake, brake };
    const auto result = gen->splice(tails, 110'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        auto edges = rises(i);
        ASSERT_EQ(edges.size(), brake->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *brake->risingTick(p));
        EXPECT_FALSE(hw->high[i]);
    }
}
TEST_F(ExecutorTest, SpliceCruiseRetainsPrefixAndFinishesLocalBrake)
{
    hw->advance(2'510'000);
    auto brake = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    ASSERT_TRUE(brake->valid());
    auto result = gen->splice({ brake, brake, brake }, 2'610'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        auto edges = rises(i);
        ASSERT_EQ(edges.size(), brake->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *brake->risingTick(p));
        EXPECT_EQ(out[i]->status().state, hal::step::State::Completed);
    }
}
TEST_F(ExecutorTest, LateOrDifferentPrefixRejectsEveryAxisWithoutMutation)
{
    hw->advance(2'510'000);
    auto good = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    auto wrong = std::make_shared<Schedule>(*nominal, 1'000'000, limits);
    const auto saved = hw->memory;
    EXPECT_FALSE(gen->splice({ good, good, wrong }, 2'610'000));
    EXPECT_EQ(saved, hw->memory);
    EXPECT_FALSE(gen->splice({ good, good, good }, 2'515'000));
    EXPECT_EQ(saved, hw->memory);
    hw->advance(7'000'000);
    EXPECT_EQ(rises(0).size(), 10000U);
    EXPECT_EQ(rises(1).size(), 10000U);
    EXPECT_EQ(rises(2).size(), 10000U);
}

#include "io/motion/Executor.hpp"
TEST_F(ExecutorTest, RetainedBankReportsStorageFailureInsteadOfMissedDeadline)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    std::shared_ptr<const hal::step::Sequence> retained;
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        if (i==0) retained=s;
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command command{.id=1};
    command.axes[0].profile=nominal->profile().description();
    command.axes[0].limits=limits;
    ASSERT_TRUE(exec.submit(command));
    for (unsigned ms=0; ms<1000 && exec.active(); ++ms) {
        hw->advance(10'000);
        exec.renew(command.id);
        exec.service();
    }
    const auto status=exec.status();
    EXPECT_EQ(status.state, io::motion::Executor::State::Stopped);
    EXPECT_EQ(status.reason, io::motion::Executor::Reason::ScheduleRejected);
    EXPECT_EQ(status.last_error, int(std::errc::device_or_resource_busy));
    EXPECT_EQ(status.diagnostic.failure, io::motion::ScheduleFailure::BankInUse);
    EXPECT_EQ(status.diagnostic.axis, 0U);
    EXPECT_GT(status.diagnostic.cut, status.stopping_at);
    EXPECT_GT(status.diagnostic.state.position, 0);
    EXPECT_LT(status.pulses[0], 10000U);
    retained.reset();
    ++command.id;
    ASSERT_TRUE(exec.prepare(command));
    EXPECT_EQ(exec.status().diagnostic.failure, io::motion::ScheduleFailure::None);
    exec.abort();
}
TEST_F(ExecutorTest, CapturedProgramCompletesAndStopsWithOneCommonTimeline)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command command;
    command.progress_limits = {1.7777777777768662, 7.1111111111068084,
                               7.1111111111068084, 56.888888888849216};
    const auto law = motion::Profile::create(1., command.progress_limits);
    ASSERT_TRUE(law);
    command.progress = law->description();
    for (auto& axis : command.axes) {
        axis.profile = law->scaled(400)->description();
        axis.limits = {400*command.progress_limits.velocity, 400*command.progress_limits.acceleration,
                       400*command.progress_limits.deceleration, 400*command.progress_limits.jerk};
    }
    // Exercise either side of renewal and initial stop boundaries as well as
    // acceleration, cruise and deceleration. Counts come from the independent
    // profile/brake evaluation below, not from the executor's reported count.
    for (const unsigned stopAt : {0U, 39U, 40U, 41U, 79U, 80U, 81U, 160U, 450U, 750U}) {
        hw->edges.clear();
        ++command.id;
        ASSERT_TRUE(exec.submit(command));
        std::uint64_t frontier{};
        for (unsigned ms=1; ms<1500 && exec.active(); ++ms) {
            hw->advance(10'000);
            if (ms==stopAt) {
                frontier=exec.status().stopping_at;
                ASSERT_TRUE(exec.stop(command.id));
            } else if (!stopAt || ms<stopAt) {
                ASSERT_TRUE(exec.renew(command.id));
            }
            exec.service();
        }
        const auto status=exec.status();
        ASSERT_EQ(status.state, stopAt ? io::motion::Executor::State::Stopped : io::motion::Executor::State::Completed)
          << stopAt << ": failure=" << unsigned(status.diagnostic.failure);
        EXPECT_EQ(status.reason, stopAt ? io::motion::Executor::Reason::Cancelled : io::motion::Executor::Reason::None);
        EXPECT_EQ(status.diagnostic.failure, io::motion::ScheduleFailure::None);
        unsigned count=400;
        if (stopAt && !law->brakingToRest(frontier/1e7)) {
            const auto state=law->at(frontier/1e7);
            const auto brake=motion::Profile::brake(state.velocity,state.acceleration,command.progress_limits);
            ASSERT_TRUE(brake);
            count=unsigned(std::floor(400*(state.position+brake->distance())));
        }
        ASSERT_EQ(rises(0).size(), count);
        EXPECT_EQ(rises(0), rises(1));
        EXPECT_EQ(rises(0), rises(2));
        std::array<std::uint64_t,3> previous{};
        for (const auto& edge : hw->edges) {
            if (edge.high) previous[edge.axis]=edge.tick;
            else EXPECT_EQ(edge.tick-previous[edge.axis], 50U);
        }
    }
}
TEST_F(ExecutorTest, RenewedFirstPulseIncludesDmaPrefetch)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command command{ .id = 1 };
    for (unsigned i = 0; i < 3; ++i) {
        const auto rate = std::array{ 12500., 10000., 6250. }[i];
        command.axes[i].limits = { rate, rate * 10, rate * 10, rate * 100 };
        command.axes[i].profile = motion::Profile::create(.4 * rate, command.axes[i].limits)->description();
    }
    ASSERT_TRUE(exec.submit(command));
    for (unsigned t = 0; t < 700 && exec.active(); ++t) {
        hw->advance(10'000);
        exec.renew(command.id);
        exec.service();
    }
    ASSERT_EQ(exec.status().state, io::motion::Executor::State::Completed);
    std::array<std::uint64_t, 3> rise{};
    std::array<unsigned, 3> counts{};
    for (const auto& edge : hw->edges) {
        if (edge.high) {
            rise[edge.axis] = edge.tick;
            ++counts[edge.axis];
        }
        else {
            EXPECT_EQ(edge.tick - rise[edge.axis], 50U)
              << "axis=" << edge.axis << " pulse=" << counts[edge.axis];
        }
    }
    EXPECT_EQ(counts, (std::array<unsigned, 3>{ 5000, 4000, 2500 }));
}
TEST_F(ExecutorTest, ProductionExecutorRenewsToFullCompletion)
{
    gen->stop();
    ASSERT_TRUE(gen->setRefillNotification([] {}));
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command command{ .id = 1 };
    for (auto& a : command.axes) {
        a.profile = nominal->profile().description();
        a.limits = limits;
    }
    ASSERT_TRUE(exec.submit(command));
    EXPECT_EQ(hw->entries[0], 128U); // Initial short brake must not shrink steady-state DMA capacity.
    for (unsigned t = 0; t < 800 && exec.active(); ++t) {
        hw->advance(10'000);
        if (t % 5 == 0) {
            const auto renewed = exec.renew(1);
            ASSERT_TRUE(renewed || !exec.active()) << unsigned(exec.status().reason) << " at " << t;
        }
        exec.service();
    }
    auto status = exec.status();
    EXPECT_EQ(status.state, io::motion::Executor::State::Completed)
      << unsigned(status.reason) << " at " << status.elapsed;
    EXPECT_EQ(status.renewals, 13U); // 40 ms renewals + 80 ms frontier cover the 600 ms endpoint.
    EXPECT_EQ(status.pulses, (std::array<std::uint64_t, 3>{ 10000, 10000, 10000 }));
}
TEST_F(ExecutorTest, LegacyInterruptRefillFinishesBrakeWithoutExecutorWorker)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command command{ .id = 1 };
    for (auto& a : command.axes) {
        a.profile = nominal->profile().description();
        a.limits = limits;
    }
    ASSERT_TRUE(exec.submit(command));
    for (unsigned t = 0; t < 250; ++t) {
        hw->advance(10'000);
        ASSERT_TRUE(exec.renew(1));
        exec.service();
    }
    const auto cut = exec.status().stopping_at;
    // Compatibility mode without a refill notifier. Production uses the worker;
    // its disappearance is a guard fault, tested separately below.
    hw->advance(8'000'000);
    exec.service();
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Stopped);
    EXPECT_LT(exec.status().pulses[0], 10000U);
    EXPECT_GT(exec.status().pulses[0], 1000U);
    auto brake = std::make_shared<Schedule>(*nominal, cut, limits);
    ASSERT_TRUE(brake->valid());
    EXPECT_EQ(exec.status().pulses[0], brake->count());
    EXPECT_FALSE(exec.renew(1));
    EXPECT_FALSE(exec.submit(command));
    command.id = 2;
    ASSERT_TRUE(exec.submit(command));
    EXPECT_TRUE(exec.stop(2));
    hw->advance(8'000'000);
    exec.service();
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Stopped);
}
TEST(ProfileSchedule, EveryRenewalBoundaryBuildsAValidStop)
{
    auto profile = *motion::Profile::create(10000, limits);
    Schedule nominal(profile);
    for (unsigned cut = 50'000; cut < 6'000'000; cut += 531) {
        Schedule braking(nominal, cut, limits);
        EXPECT_TRUE(braking.valid()) << cut << " pos=" << profile.at(cut / 1e7).position;
    }
}

TEST_F(ExecutorTest, VerySlowStartKeepsZeroPulseBrakeUntilRenewed)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    c.axes[0].limits = { 100, 100, 100, 1000 };
    c.axes[0].profile = motion::Profile::create(10, c.axes[0].limits)->description();
    ASSERT_TRUE(exec.submit(c));
    EXPECT_EQ(out[0]->status().pulses, 0U);
    for (unsigned t = 0; t < 1000 && exec.active(); ++t) {
        hw->advance(10'000);
        exec.renew(1);
        exec.service();
    }
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Completed);
    EXPECT_EQ(exec.status().pulses[0], 10U);
    c.id = 2;
    ASSERT_TRUE(exec.submit(c));
    ASSERT_TRUE(exec.stop(2));
    hw->advance(10'000'000);
    exec.service();
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Stopped);
    EXPECT_EQ(exec.status().pulses[0], 0U);
}
TEST_F(ExecutorTest, InvalidProfilesNeverEmitAndOldIdsCannotRestart)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    c.axes[0].limits = limits;
    c.axes[0].profile = nominal->profile().description();
    c.axes[0].limits.velocity = 25'001;
    EXPECT_FALSE(exec.submit(c));
    hw->advance(1'000'000);
    EXPECT_TRUE(hw->edges.empty());
    c.axes[0].limits = limits;
    ASSERT_TRUE(exec.submit(c));
    exec.abort();
    EXPECT_FALSE(exec.submit(c));
    ASSERT_TRUE(gen->start());
    ASSERT_TRUE(exec.recover());
    EXPECT_FALSE(exec.submit(c));
    c.id = 2;
    ASSERT_TRUE(exec.submit(c));
    EXPECT_FALSE(exec.renew(1));
    EXPECT_FALSE(exec.stop(1));
    EXPECT_TRUE(exec.stop(2));
}
TEST_F(ExecutorTest, FailedPreparationReleasesAllUnstartedSchedules)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    std::weak_ptr<const hal::step::Sequence> retained;
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        retained = s;
        EXPECT_TRUE(out[i]->prepareSequence(s));
        return false; // E.g. a driver/reference condition changes during preparation.
    });
    io::motion::Executor::Command command{ .id = 1 };
    command.axes[0].profile = nominal->profile().description();
    command.axes[0].limits = limits;
    EXPECT_FALSE(exec.prepare(command));
    EXPECT_TRUE(retained.expired());
    EXPECT_FALSE(exec.reserved());
    EXPECT_EQ(out[0]->status().state, hal::step::State::Idle);
    hw->advance(8'000'000);
    EXPECT_TRUE(rises(0).empty());
}
TEST_F(ExecutorTest, PreparedProgramCannotPulseUntilExplicitCommit)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    c.axes[0].limits = limits;
    c.axes[0].profile = nominal->profile().description();
    ASSERT_TRUE(exec.prepare(c));
    hw->advance(2'000'000);
    EXPECT_TRUE(hw->edges.empty());
    EXPECT_FALSE(exec.start(2));
    EXPECT_TRUE(exec.stop(1));
    EXPECT_FALSE(exec.start(1));
    c.id = 2;
    ASSERT_TRUE(exec.prepare(c));
    ASSERT_TRUE(exec.start(2));
    EXPECT_FALSE(exec.submit(c));
    hw->advance(8'000'000);
    exec.service();
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Stopped);
}

TEST_F(ExecutorTest, DifferentDurationsReleaseTheirBanksWithoutStoppingOtherAxes)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    const std::array<unsigned, 3> counts{ 100, 1700, 10000 };
    for (unsigned i = 0; i < 3; ++i) {
        c.axes[i].limits = limits;
        c.axes[i].profile = motion::Profile::create(counts[i], limits)->description();
    }
    ASSERT_TRUE(exec.submit(c));
    for (unsigned t = 0; t < 1000 && exec.active(); ++t) {
        hw->advance(10'000);
        exec.renew(1);
        exec.service();
    }
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Completed) << unsigned(exec.status().reason);
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_EQ(exec.status().pulses[i], counts[i]);
}
namespace
{
    class AdvancingSchedule final : public hal::step::TimedSequence
    {
      public:
        std::shared_ptr<Schedule> value;
        std::function<void()> advance;
        bool certificate{ true };
        std::uint32_t window{ 64 };
        mutable bool advanced{};
        std::uint32_t bufferPulses() const noexcept override { return window; }
        hal::step::PulseCount count() const noexcept override { return value->count(); }
        std::chrono::nanoseconds maximumPeriod() const noexcept override { return value->maximumPeriod(); }
        hal::util::Result<hal::step::Timing> timing(hal::step::PulseCount p) const noexcept override
        {
            return value->timing(p);
        }
        hal::util::Result<> generate(hal::step::PulseCount p,
                                     std::span<hal::step::Timing> t) const noexcept override
        {
            if (!certificate && !advanced) {
                advanced = true;
                advance();
            }
            return value->generate(p, t);
        }
        hal::util::Result<std::uint32_t> risingTick(hal::step::PulseCount p) const noexcept override
        {
            return value->risingTick(p);
        }
        std::optional<std::int64_t> endTickOffset() const noexcept override { return value->endTickOffset(); }
        std::optional<std::int64_t> commonPrefix(const hal::step::Sequence& previous) const noexcept override
        {
            if (!certificate)
                return {};
            advance();
            return value->commonPrefix(previous);
        }
    };
}
TEST_F(ExecutorTest, StagingStorageHandlesDifferentPreparedWindowSizes)
{
    for (const unsigned window : { 16U, 256U, 64U }) {
        gen->stop();
        ASSERT_TRUE(gen->start());
        auto sized = std::make_shared<AdvancingSchedule>();
        sized->value = nominal;
        sized->window = window;
        sized->advance = [] {};
        std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
        for (unsigned i = 0; i < 3; ++i) {
            ASSERT_TRUE(out[i]->prepareSequence(i ? std::shared_ptr<const hal::step::Sequence>(nominal) : sized));
            delays[i] = 1ms + std::chrono::nanoseconds(nominal->firstTick() * 100ULL);
        }
        ASSERT_TRUE(gen->startPrepared(delays));
        hw->advance(2'510'000);
        auto brake = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
        ASSERT_TRUE(gen->splice({ brake, {}, {} }, 2'610'000));
        hw->advance(7'000'000);
        EXPECT_EQ(rises(0).size(), brake->count());
        EXPECT_EQ(rises(1).size(), nominal->count());
        EXPECT_EQ(rises(2).size(), nominal->count());
    }
}
TEST_F(ExecutorTest, IdenticalQueuedPrefixSurvivesDmaTurnoverDuringPreparation)
{
    hw->advance(2'510'000);
    auto tail = std::make_shared<Schedule>(*nominal, 3'600'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->advance = [this] { hw->advance(30'000); };
    const auto result = gen->splice({ advancing, tail, tail }, 3'310'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        auto edges = rises(i);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *tail->risingTick(p));
    }
}
TEST_F(ExecutorTest, DiscontinuousAccelerationIsNotAJerkLimitedCommand)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    motion::Profile::Description d;
    d.size = 3;
    d.distance = 10;
    d.end_velocity = 0;
    d.phases[0] = { .duration = 1, .jerk = 24, .position = 0, .velocity = 0, .acceleration = 0 };
    d.phases[1] = { .duration = 1, .jerk = 0, .position = 4, .velocity = 12, .acceleration = -12 };
    d.phases[2] = { .duration = 1, .jerk = 0, .position = 10, .velocity = 0, .acceleration = 0 };
    c.axes[0].limits = limits;
    c.axes[0].profile = d;
    EXPECT_FALSE(exec.prepare(c));
    hw->advance(1'000'000);
    EXPECT_TRUE(hw->edges.empty());
}
TEST_F(ExecutorTest, ExecutionAndBrakingCrossTimerWrap)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    hw->advance(hal::util::STEP_PARK - 2'000'000ULL);
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    c.axes[0].limits = limits;
    c.axes[0].profile = nominal->profile().description();
    ASSERT_TRUE(exec.submit(c));
    for (unsigned t = 0; t < 1000 && exec.active(); ++t) {
        hw->advance(10'000);
        exec.renew(1);
        exec.service();
    }
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Completed);
    EXPECT_EQ(exec.status().pulses[0], 10000U);
}
TEST_F(ExecutorTest, ReferenceIndependentHardwareGuardStillStopsWhenInterruptsDisappear)
{
    hw->interrupts_enabled = false;
    hw->advance(8'000'000);
    EXPECT_FALSE(hw->registers.running);
    for (auto high : hw->high)
        EXPECT_FALSE(high);
    gen->service();
    EXPECT_NE(gen->status().state, hal::step::State::Running);
}
TEST_F(ExecutorTest, ChangedFutureBuffersSurviveOneDmaTurnoverDuringPreparation)
{
    hw->advance(2'510'000);
    auto tail = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->certificate = false;
    advancing->advance = [this] { hw->advance(30'000); };
    const auto result = gen->splice({ advancing, tail, tail }, 2'610'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        auto edges = rises(i);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *tail->risingTick(p));
    }
}

TEST_F(ExecutorTest, ChangedFutureBuffersSurviveTwoDmaTurnoversDuringPreparation)
{
    hw->advance(2'510'000);
    auto tail = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->certificate = false;
    advancing->advance = [this] { hw->advance(51'200); };
    const auto result = gen->splice({ advancing, tail, tail }, 2'610'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        const auto edges = rises(i);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *tail->risingTick(p));
    }
}

TEST_F(ExecutorTest, ThreeDmaTurnoversRejectPublicationAndKeepOriginalWaveform)
{
    hw->advance(2'510'000);
    auto tail = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->certificate = false;
    advancing->advance = [this] { hw->advance(76'800); };
    const auto result = gen->splice({ advancing, tail, tail }, 2'610'000);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::resource_unavailable_try_again);
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        const auto edges = rises(i);
        ASSERT_EQ(edges.size(), nominal->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *nominal->risingTick(p));
    }
}

TEST_F(ExecutorTest, TurnoverDuringFinalServiceIsAccountedWithoutRebuildingTheTransaction)
{
    hw->advance(2'510'000);
    bool inject{};
    progress = [&] {
        if (std::exchange(inject, false))
            hw->advance(25'600); // DMA progresses, IRQ service still masked.
    };
    auto tail = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->certificate = false;
    advancing->advance = [&] {
        hw->interrupts_enabled = false;
        hw->advance(400);
        inject = true;
    };
    const auto result = gen->splice({ advancing, tail, tail }, 2'610'000);
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_FALSE(inject);
    progress = {};
    hw->interrupts_enabled = true;
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        const auto edges = rises(i);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *tail->risingTick(p));
    }
}

TEST_F(ExecutorTest, ChangedDmaTargetWithoutObservedFlagResamplesTheStagedTransaction)
{
    hw->advance(2'510'000);
    bool inject{}, hideFlag{};
    progress = [&] {
        if (std::exchange(inject, false)) {
            hw->advance(25'600);
            hideFlag = true;
        }
    };
    hw->sample_filter = [&](auto& sample) {
        if (std::exchange(hideFlag, false))
            for (auto& channel : sample.channels)
                channel.transfer_complete = false;
    };
    auto tail = std::make_shared<Schedule>(*nominal, 2'600'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->certificate = false;
    advancing->advance = [&] {
        hw->interrupts_enabled = false;
        hw->advance(400);
        inject = true;
    };
    const auto result = gen->splice({ advancing, tail, tail }, 2'610'000);
    EXPECT_TRUE(result) << result.error().message();
    EXPECT_FALSE(inject);
    EXPECT_FALSE(hideFlag);
    progress = {};
    hw->sample_filter = {};
    hw->interrupts_enabled = true;
    hw->advance(7'000'000);
    for (unsigned i = 0; i < 3; ++i) {
        const auto edges = rises(i);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned p = 0; p < edges.size(); ++p)
            EXPECT_EQ(edges[p], 10'000 + nominal->firstTick() + *tail->risingTick(p));
    }
}

#include <random>
TEST(ProfileSchedule, FractionalStopsMatchDoublePrecisionReferenceAcrossVariedProfiles)
{
    std::mt19937 random{ 197 };
    for (unsigned trial = 0; trial < 150; ++trial) {
        const motion::Profile::Limits l{ 1000. + random() % 24001,
                                         10000. + random() % 240001,
                                         10000. + random() % 240001,
                                         200000. + random() % 2300001 };
        const auto profile = motion::Profile::create(100 + random() % 9901, l);
        ASSERT_TRUE(profile);
        Schedule original(*profile);
        ASSERT_TRUE(original.valid());
        const auto cut = std::uint32_t((.03 + (random() % 900) / 1000.) * original.endTick());
        Schedule stop(original, cut, l);
        ASSERT_TRUE(stop.valid()) << trial;
        const auto state = profile->at(cut / 1e7);
        const auto brake = motion::Profile::brake(state.velocity, state.acceleration, l);
        // In the final monotone deceleration the original certified tail is retained.
        const bool originalTail = state.acceleration < 0;
        ASSERT_TRUE(brake || originalTail);
        for (unsigned pulse = 0; pulse < stop.count();
             pulse += std::max<unsigned>(1, unsigned(stop.count() / 17))) {
            auto expected = profile->timeAt(pulse + 1.);
            if (!originalTail && pulse + 1. > state.position) {
                const auto target = pulse + 1. - state.position;
                expected = cut / 1e7 + brake->timeAt(target);
                // Near a zero-speed endpoint, compare with the analytic rest
                // time: double position inversion is ill-conditioned there.
                if (pulse + 1 == stop.count() &&
                    std::abs(brake->distance() - target) <=
                      32 * std::numeric_limits<double>::epsilon() * std::max(1., brake->distance()))
                    expected = cut / 1e7 + brake->duration();
            }
            const auto edge = stop.risingTick(pulse);
            ASSERT_TRUE(edge);
            EXPECT_NEAR(original.firstTick() + *edge, std::ceil(expected * 1e7), 2)
              << trial << " pulse " << pulse;
        }
        if (stop.count()) {
            std::array<hal::step::Timing, 1> t;
            EXPECT_TRUE(stop.generate(stop.count() - 1, t));
        }
    }
}
TEST_F(ExecutorTest, NewCommandCountsExcludePreviouslyUsedUnselectedAxes)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    });
    io::motion::Executor::Command c{ .id = 1 };
    for (auto& a : c.axes) {
        a.limits = limits;
        a.profile = motion::Profile::create(100, limits)->description();
    }
    ASSERT_TRUE(exec.submit(c));
    for (unsigned t = 0; t < 1000 && exec.active(); ++t) {
        hw->advance(10'000);
        exec.renew(c.id);
        exec.service();
    }
    ASSERT_EQ(exec.status().state, io::motion::Executor::State::Completed);
    c.id = 2;
    c.axes[1].profile.reset();
    c.axes[2].profile.reset();
    ASSERT_TRUE(exec.submit(c));
    for (unsigned t = 0; t < 1000 && exec.active(); ++t) {
        hw->advance(10'000);
        exec.renew(c.id);
        exec.service();
    }
    EXPECT_EQ(exec.status().state, io::motion::Executor::State::Completed);
    EXPECT_EQ(exec.status().pulses, (std::array<std::uint64_t, 3>{ 100, 0, 0 }));
}

TEST(ProfileSchedule, NearIntegerBrakeEndpointOccursAtRestWithoutAddingAPulse)
{
    const motion::Profile::Limits limits{25000, 250000, 250000, 2500000};
    const auto profile = *motion::Profile::create(10000, limits);
    const Schedule nominal(profile);
    // Reproduces the lost-keepalive hardware capture: this stop ends within
    // floating-point roundoff of 5000 microsteps, at zero velocity.
    for (const auto cut : {1029251U, 1000000U, 1500000U}) {
        const auto state = profile.at(cut / 1e7);
        const auto brake = *motion::Profile::brake(state.velocity, state.acceleration, limits);
        const Schedule stopped(nominal, cut, limits);
        ASSERT_TRUE(stopped.valid());
        EXPECT_EQ(stopped.count(), std::uint64_t(std::floor(state.position + brake.distance())));
        if (stopped.count() == 5000) {
            ASSERT_TRUE(stopped.risingTick(4999));
            EXPECT_EQ(nominal.firstTick() + *stopped.risingTick(4999), stopped.endTick());
        }
    }
}

TEST_F(ExecutorTest, MixedAxisRenewalStagesAllBuffersBeforeDmaTurnover)
{
    hw->advance(2'510'000);
    auto tail = std::make_shared<Schedule>(*nominal, 2'560'000, limits);
    auto expensive = std::make_shared<AdvancingSchedule>();
    expensive->value = tail;
    expensive->certificate = false;
    expensive->advance = [this] { hw->advance(30'000); };
    // Axes 0/1 initially qualify for pointer-only publication. While axis 2
    // evaluates, their DMA refill crosses the certificate boundary. All axes
    // must have a staged fallback instead of repeatedly rejecting the command.
    const auto result = gen->splice({tail, tail, expensive}, 2'570'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto edges = rises(axis);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned pulse = 0; pulse < edges.size(); ++pulse)
            EXPECT_EQ(edges[pulse], 10'000 + nominal->firstTick() + *tail->risingTick(pulse));
    }
}

TEST_F(ExecutorTest, PointerOnlyCandidateStagesBeforeItsPrefixExpiresAtNextRefill)
{
    hw->advance(2'510'000);
    auto tail = std::make_shared<Schedule>(*nominal, 2'560'000, limits);
    auto advancing = std::make_shared<AdvancingSchedule>();
    advancing->value = tail;
    advancing->advance = [this] { hw->advance(30'000); };
    // Every axis initially has two queued halves within the certificate. The
    // next refill exceeds it. Anticipate that refill and stage the transaction
    // rather than retrying the same pointer-only certificate until it is late.
    const auto result = gen->splice({ advancing, tail, tail }, 2'570'000);
    ASSERT_TRUE(result) << result.error().message();
    hw->advance(7'000'000);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto edges = rises(axis);
        ASSERT_EQ(edges.size(), tail->count());
        for (unsigned pulse = 0; pulse < edges.size(); ++pulse)
            EXPECT_EQ(edges[pulse], 10'000 + nominal->firstTick() + *tail->risingTick(pulse));
    }
}

TEST_F(ExecutorTest, AxisCompletingBeforeSpliceRequestsARefreshedSnapshot)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    auto shortAxis = std::make_shared<Schedule>(*motion::Profile::create(1200, limits));
    std::array<std::optional<std::chrono::nanoseconds>, 3> delay;
    for (unsigned axis = 0; axis < 3; ++axis) {
        auto schedule = axis == 2 ? shortAxis : nominal;
        ASSERT_TRUE(out[axis]->prepareSequence(schedule));
        delay[axis] = 1ms + std::chrono::nanoseconds(schedule->firstTick() * 100ULL);
    }
    ASSERT_TRUE(gen->startPrepared(delay));
    hw->advance(2'600'000);
    ASSERT_EQ(gen->status().axes[2], hal::step::State::Completed);
    auto tail = std::make_shared<Schedule>(*nominal, 3'000'000, limits);
    const auto stale = gen->splice({tail, tail, shortAxis}, 3'010'000);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error(), std::errc::resource_unavailable_try_again);
    ASSERT_TRUE(out[2]->clear());
    ASSERT_TRUE(gen->splice({tail, tail, nullptr}, 3'010'000));
    hw->advance(7'000'000);
    EXPECT_EQ(rises(0).size(), tail->count());
    EXPECT_EQ(rises(1).size(), tail->count());
    EXPECT_EQ(rises(2).size(), 1200U);
}

#include "io/motion/ProfileMemory.hpp"
TEST(ProfileMemory, BoundedAlignedAndReusableWithoutHeapFallback)
{
    io::motion::ProfileMemory memory;
    std::array<void*, 9> slots;
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        for (unsigned i = 0; i < slots.size(); ++i) {
            const auto size = 2800U;
            slots[i] = memory.allocate(size, 32);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(slots[i]) % 32, 0U);
            for (unsigned j = 0; j < i; ++j)
                EXPECT_NE(slots[i], slots[j]);
        }
        EXPECT_THROW((void)memory.allocate(8400, 32), std::bad_alloc);
        EXPECT_THROW((void)memory.allocate(2800, 32), std::bad_alloc);
        for (unsigned i = 0; i < slots.size(); ++i)
            memory.deallocate(slots[i], 2800U, 32);
    }
    EXPECT_THROW((void)memory.allocate(9000, 32), std::bad_alloc);
    EXPECT_THROW((void)memory.allocate(100, 64), std::bad_alloc);
}
TEST_F(ExecutorTest, LocalProfileStorageSurvivesCompletionAndCancellationReuse)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    io::motion::ProfileMemory memory;
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    }, &memory);
    io::motion::Executor::Command command;
    for (auto& axis : command.axes) {
        axis.limits = limits;
        axis.profile = nominal->profile().description();
    }
    for (unsigned n = 1; n <= 6; ++n) {
        command.id = n;
        ASSERT_TRUE(exec.submit(command));
        for (unsigned t = 0; t < 1000 && exec.active(); ++t) {
            hw->advance(10'000);
            if (n % 2 && t == 100)
                ASSERT_TRUE(exec.stop(n));
            else
                exec.renew(n);
            exec.service();
        }
        EXPECT_EQ(exec.status().state, n % 2 ? io::motion::Executor::State::Stopped
                                             : io::motion::Executor::State::Completed);
    }
}
TEST_F(ExecutorTest, ExhaustedLocalStorageRollsBackBeforeMotionAndCanRetry)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    hw->edges.clear();
    io::motion::ProfileMemory memory;
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(s));
    }, &memory);
    io::motion::Executor::Command command{ .id = 1 };
    for (auto& axis : command.axes) {
        axis.limits = limits;
        axis.profile = nominal->profile().description();
    }
    auto* occupied = memory.allocate(2800, 32);
    EXPECT_THROW((void)exec.prepare(command), std::bad_alloc);
    EXPECT_FALSE(exec.reserved());
    hw->advance(1'000'000);
    EXPECT_TRUE(hw->edges.empty());
    memory.deallocate(occupied, 2800, 32);
    ASSERT_TRUE(exec.prepare(command));
    EXPECT_TRUE(exec.stop(command.id));
    EXPECT_TRUE(hw->edges.empty());
}

TEST(ProfileSchedule, NearlyWholeStepOffsetRetainsTheActualSlowCrossing)
{
    const auto profile = motion::Profile::brake(0.000001, 0, {1., 0.0001, 0.0001, 0.0001});
    ASSERT_TRUE(profile);
    const double offset = 1. - 1e-10;
    io::motion::ProfileCrossing crossing(*profile, offset);
    io::motion::profile_util::Curve reference(*profile);
    ASSERT_TRUE(crossing.valid());
    const auto tick = crossing.next(1);
    EXPECT_EQ(tick, 1001U);
    EXPECT_TRUE(reference.crossed(tick, 1 - offset));
    EXPECT_FALSE(reference.crossed(tick - 1, 1 - offset));
}

TEST_F(ExecutorTest, WorkerRefillsWithoutCallingTheSequenceFromInterrupts)
{
    gen->stop();
    bool interrupt{}, requested{};
    ASSERT_TRUE(gen->setRefillNotification([&] { requested = true; }));
    hw->interrupt = [&] { interrupt = true; gen->service(); interrupt = false; };
    ASSERT_TRUE(gen->start());
    class Checked final : public hal::step::Sequence {
      public:
        const bool& interrupt;
        mutable unsigned calls{};
        explicit Checked(const bool& in) : interrupt(in) {}
        hal::step::PulseCount count() const noexcept override { return 300; }
        std::uint32_t bufferPulses() const noexcept override { return 32; }
        std::chrono::nanoseconds maximumPeriod() const noexcept override { return 80us; }
        hal::util::Result<hal::step::Timing> timing(hal::step::PulseCount) const noexcept override {
            EXPECT_FALSE(interrupt); ++calls;
            return hal::step::Timing{80us, 5us};
        }
    };
    auto sequence = std::make_shared<Checked>(interrupt);
    ASSERT_TRUE(out[0]->prepareSequence(sequence));
    ASSERT_TRUE(out[0]->start(5ms));
    for (unsigned t = 0; t < 400; ++t) {
        hw->advance(1000);
        if (requested) { requested = false; gen->preparePending(); }
    }
    EXPECT_EQ(out[0]->status().state, hal::step::State::Completed);
    EXPECT_EQ(rises(0).size(), 300U);
    EXPECT_EQ(sequence->calls, 300U);
    std::uint64_t rise{};
    for (const auto& edge : hw->edges) {
        if (edge.high) rise = edge.tick;
        else EXPECT_EQ(edge.tick - rise, 50U);
    }
    gen->stop();
    ASSERT_TRUE(gen->setRefillNotification({}));
    hw->interrupt = [this] { gen->service(); };
}

TEST_F(ExecutorTest, MissingRefillWorkerTripsGuardInsteadOfReplayingAStaleBuffer)
{
    gen->stop();
    ASSERT_TRUE(gen->setRefillNotification([] {}));
    ASSERT_TRUE(gen->start());
    auto source = std::make_shared<Schedule::Nominal>(*motion::Profile::create(5000, limits), 32);
    auto sequence = std::make_shared<Schedule>(source);
    ASSERT_TRUE(out[0]->prepareSequence(sequence));
    ASSERT_TRUE(out[0]->start(5ms));
    hw->advance(10'000'000);
    EXPECT_EQ(gen->status().state, hal::step::State::Underrun);
    EXPECT_LE(rises(0).size(), 64U);
    gen->stop();
    ASSERT_TRUE(gen->setRefillNotification({}));
}

TEST_F(ExecutorTest, ProducerLossKeepsWorkerAliveToFinishTheInstalledBrake)
{
    gen->stop();
    ASSERT_TRUE(gen->setRefillNotification([] {}));
    ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen, [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command command{.id = 1};
    for (auto& axis : command.axes) {
        axis.profile = nominal->profile().description(); axis.limits = limits;
    }
    ASSERT_TRUE(exec.submit(command));
    for (unsigned ms = 0; ms < 1000 && exec.active(); ++ms) {
        hw->advance(10'000);
        if (ms < 250) { ASSERT_TRUE(exec.renew(command.id)); }
        exec.service();
    }
    ASSERT_EQ(exec.status().state, io::motion::Executor::State::Stopped);
    EXPECT_EQ(exec.status().reason, io::motion::Executor::Reason::LeaseExpired);
    // This tests the local executor keepalive, not the application's 250 ms UI
    // ownership lease. Expiry must select an already installed stop boundary.
    EXPECT_LT(exec.status().stopping_at, 2'500'000U +
              io::motion::Executor::LEASE + io::motion::Executor::HORIZON);
    const Schedule brake(*nominal, exec.status().stopping_at, limits);
    ASSERT_TRUE(brake.valid());
    for (unsigned axis = 0; axis < 3; ++axis) {
        EXPECT_EQ(rises(axis).size(), brake.count());
        EXPECT_FALSE(hw->high[axis]);
    }
    EXPECT_FALSE(exec.renew(command.id));
    EXPECT_FALSE(exec.start(command.id));
}

TEST_F(ExecutorTest, IndependentGroupsShareStorageAndAbortingOneLeavesOthersRunning)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    hw->edges.clear();
    io::motion::ProfileMemory memory;
    std::array<std::unique_ptr<io::motion::Executor>, 3> groups;
    for (unsigned i = 0; i < 3; ++i) {
        groups[i] = std::make_unique<io::motion::Executor>(gen,
          [&](unsigned n, bool, std::shared_ptr<const hal::step::Sequence> s) {
              return bool(out[n]->prepareSequence(std::move(s)));
          }, &memory);
        io::motion::Executor::Command c{.id = i + 1};
        c.axes[i].limits = {6250, 62500, 62500, 625000};
        c.axes[i].profile = motion::Profile::create(2500, c.axes[i].limits)->description();
        ASSERT_TRUE(groups[i]->submit(c));
    }
    for (unsigned ms = 0; ms < 1000; ++ms) {
        hw->advance(10'000);
        if (ms == 100) groups[0]->abort();
        for (unsigned i = 0; i < 3; ++i) {
            if (groups[i]->active()) groups[i]->renew(i + 1);
            groups[i]->service();
        }
    }
    EXPECT_EQ(groups[0]->status().state, io::motion::Executor::State::Faulted);
    EXPECT_LT(rises(0).size(), 2500U);
    for (unsigned i = 1; i < 3; ++i) {
        EXPECT_EQ(groups[i]->status().state, io::motion::Executor::State::Completed);
        EXPECT_EQ(rises(i).size(), 2500U);
    }
}

TEST_F(ExecutorTest, CoordinatedGroupUsesCommonProgressForNominalAndStopping)
{
    gen->stop();
    ASSERT_TRUE(gen->start());
    hw->edges.clear();
    io::motion::ProfileMemory memory;
    io::motion::Executor exec(gen,
      [&](unsigned i, bool, std::shared_ptr<const hal::step::Sequence> s) {
          return bool(out[i]->prepareSequence(std::move(s)));
      }, &memory);
    io::motion::Executor::Command c{.id = 1};
    c.progress_limits = {1., 10., 10., 100.};
    const auto law = motion::Profile::create(1., c.progress_limits);
    ASSERT_TRUE(law);
    c.progress = law->description();
    const std::array<double, 3> distances{4000, 2000, 1000};
    for (unsigned i = 0; i < 3; ++i) {
        const auto d = distances[i];
        c.axes[i].limits = {d, d*10, d*10, d*100};
        c.axes[i].profile = law->scaled(d)->description();
    }
    ASSERT_TRUE(exec.submit(c));
    std::uint32_t frontier{};
    for (unsigned ms = 0; ms < 1500 && exec.active(); ++ms) {
        hw->advance(10'000);
        if (ms == 250) {
            frontier = exec.status().stopping_at;
            ASSERT_TRUE(exec.stop(c.id));
        } else if (ms < 250) { ASSERT_TRUE(exec.renew(c.id)); }
        exec.service();
    }
    ASSERT_EQ(exec.status().state, io::motion::Executor::State::Stopped);
    const auto state = law->at(frontier / 1e7);
    const auto brake = motion::Profile::brake(state.velocity, state.acceleration, c.progress_limits);
    ASSERT_TRUE(brake);
    const auto end = state.position + brake->distance();
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_EQ(rises(i).size(), std::uint64_t(std::floor(end * distances[i])));
    // All axes use the same epoch: coincident integer crossings agree exactly.
    const auto a = rises(0), b = rises(1), d = rises(2);
    for (unsigned n = 0; n < d.size(); ++n) {
        EXPECT_EQ(d[n], b[2*n+1]);
        EXPECT_EQ(d[n], a[4*n+3]);
    }
}

TEST(ProfileSchedule, LongFiniteProgramRetainsReferenceCrossings)
{
    const motion::Profile::Limits slow{100, 1000, 1000, 10000};
    const auto p = motion::Profile::create(8800, slow);
    ASSERT_TRUE(p);
    ASSERT_GT(p->duration(), 88.);
    ASSERT_LT(p->duration(), 90.);
    Schedule sequence(*p);
    ASSERT_TRUE(sequence.valid());
    io::motion::profile_util::Curve reference(*p);
    for (unsigned n = 0; n < 8800; ++n) {
        const auto offset = sequence.risingTick(n);
        ASSERT_TRUE(offset);
        const auto tick = sequence.firstTick() + *offset;
        EXPECT_TRUE(reference.crossed(tick, n+1));
        EXPECT_FALSE(reference.crossed(tick-1, n+1));
    }
    const auto tooLong = motion::Profile::create(9000, slow);
    ASSERT_TRUE(tooLong);
    EXPECT_FALSE(Schedule(*tooLong).valid());
}

TEST_F(ExecutorTest, ShortAxisMoveRenewsThroughFinalDeceleration)
{
    gen->stop(); ASSERT_TRUE(gen->start()); hw->edges.clear();
    io::motion::Executor exec(gen,[&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command c{.id=1};
    constexpr double count=222;
    c.axes[0].limits={888.8888888889,4444.4444444444,4444.4444444444,44444.4444444444};
    c.progress_limits={c.axes[0].limits.velocity/count,c.axes[0].limits.acceleration/count,
                      c.axes[0].limits.deceleration/count,c.axes[0].limits.jerk/count};
    const auto law=motion::Profile::create(1,c.progress_limits);
    ASSERT_TRUE(law);c.progress=law->description();c.axes[0].profile=law->scaled(count)->description();
    ASSERT_TRUE(exec.submit(c));
    for(unsigned ms=0;ms<2000&&exec.active();++ms){
        hw->advance(10000);exec.renew(1);exec.service();
    }
    const auto s=exec.status();
    EXPECT_EQ(s.state,io::motion::Executor::State::Completed)<<unsigned(s.reason)<<" "<<s.last_error<<" "<<s.stopping_at;
    EXPECT_EQ(rises(0).size(),222U);
}

TEST_F(ExecutorTest, ThreeAxisApplicationMoveReachesCommonTerminalEdge)
{
    gen->stop(); ASSERT_TRUE(gen->setRefillNotification([]{}));ASSERT_TRUE(gen->start());hw->edges.clear();
    io::motion::Executor exec(gen,[&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command c{.id=1};
    const std::array<double,3> counts{3462,919,25380};
    c.progress_limits={5120/25380./1.25,25600/25380./1.5625,25600/25380./1.5625,256000/25380./1.953125};
    const auto law=motion::Profile::create(1,c.progress_limits);ASSERT_TRUE(law);
    c.progress=law->description();
    for(unsigned i=0;i<3;++i){c.axes[i].profile=law->scaled(counts[i])->description();
        c.axes[i].limits={6250,25600,25600,256000};}
    ASSERT_TRUE(exec.submit(c));
    for(unsigned ms=0;ms<10000&&exec.active();++ms){hw->advance(10000);exec.renew(1);exec.service();}
    const auto s=exec.status();
    EXPECT_EQ(s.state,io::motion::Executor::State::Completed)<<unsigned(s.reason)<<" "<<s.last_error<<" "<<s.stopping_at;
    for(unsigned i=0;i<3;++i)EXPECT_EQ(rises(i).size(),unsigned(counts[i]));
    gen->stop();ASSERT_TRUE(gen->setRefillNotification({}));
}

TEST_F(ExecutorTest, RollingJogRebasesWithoutRestOrCountLossAcrossTimerWrap)
{
    gen->stop();ASSERT_TRUE(gen->setRefillNotification([]{}));ASSERT_TRUE(gen->start());hw->edges.clear();
    io::motion::ProfileMemory memory;
    io::motion::Executor exec(gen,[&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
        return bool(out[i]->prepareSequence(std::move(s)));
    },&memory);
    io::motion::Executor::Command c{.id=1};c.rolling=true;
    c.progress_limits={.002,.02,.02,.2};
    const auto law=motion::Profile::create(1,c.progress_limits);ASSERT_TRUE(law);
    c.progress=law->description();c.axes[0].profile=law->scaled(50000)->description();
    c.axes[0].limits={100,1000,1000,10000};
    ASSERT_TRUE(exec.submit(c));
    std::uint64_t frontier{};
    for(unsigned ms=0;ms<455000&&exec.active();ms+=5){
        hw->advance(50000);
        if(ms==450000){frontier=exec.status().stopping_at;ASSERT_TRUE(exec.stop(1));}
        else if(ms<450000){ASSERT_TRUE(exec.renew(1))<<ms<<" "<<unsigned(exec.status().reason)<<" "<<exec.status().last_error;}
        exec.service();
    }
    const auto s=exec.status();
    ASSERT_EQ(s.state,io::motion::Executor::State::Stopped)<<unsigned(s.reason)<<" "<<s.last_error;
    ASSERT_GT(frontier,std::uint64_t(hal::util::STEP_PARK));
    const auto state=law->at(frontier/1e7);
    const auto brake=motion::Profile::brake(state.velocity,state.acceleration,c.progress_limits);ASSERT_TRUE(brake);
    EXPECT_EQ(rises(0).size(),std::uint64_t(std::floor((state.position+brake->distance())*50000)));
    const auto edges=rises(0);
    for(unsigned n=20;n+20<edges.size();++n)EXPECT_EQ(edges[n]-edges[n-1],100000U)<<n;
    gen->stop();ASSERT_TRUE(gen->setRefillNotification({}));
}

TEST(ProfileSchedule, ExtendedJogCrossingsAgreeWithReferenceIncludingFinalRamp)
{
    const auto profile=motion::Profile::create(50000,{100,1000,1000,10000});ASSERT_TRUE(profile);
    io::motion::ProfileCrossing crossing(*profile,0,true);ASSERT_TRUE(crossing.valid());
    io::motion::profile_util::Curve reference(*profile);
    io::motion::ProfileCrossing::Cursor cursor;
    for(unsigned n=1;n<=50000;++n){
        const auto tick=crossing.next(n,cursor);
        EXPECT_TRUE(reference.crossed(tick,n))<<n;
        EXPECT_FALSE(reference.crossed(tick-1,n))<<n;
    }
}

TEST_F(ExecutorTest, SlowLowCountAxisRetainsItsFinalPulseAndCompletion)
{
    gen->stop(); ASSERT_TRUE(gen->start()); hw->edges.clear();
    io::motion::Executor exec(gen,[&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command c{.id=1};
    constexpr double count=22;
    c.axes[1].limits={22.2222222222222,4444.4444444444,4444.4444444444,44444.4444444444};
    c.progress_limits={c.axes[1].limits.velocity/count,c.axes[1].limits.acceleration/count,
                      c.axes[1].limits.deceleration/count,c.axes[1].limits.jerk/count};
    const auto law=motion::Profile::create(1,c.progress_limits);
    ASSERT_TRUE(law);c.progress=law->description();c.axes[1].profile=law->scaled(count)->description();
    for(unsigned n=1;n<=10;++n){
        c.id=n;ASSERT_TRUE(exec.submit(c));
        for(unsigned ms=0;ms<2000&&exec.active();++ms){hw->advance(10000);exec.renew(n);exec.service();}
        ASSERT_EQ(exec.status().state,io::motion::Executor::State::Completed)<<n<<" "<<unsigned(exec.status().reason);
        EXPECT_EQ(exec.status().pulses[1],22U);
    }
}

TEST_F(ExecutorTest, CompletionNotificationDefersBusyCleanupWithoutFaulting)
{
    gen->stop(); ASSERT_TRUE(gen->start()); hw->edges.clear();
    io::motion::Executor exec(gen,[&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command c{.id=1};
    c.axes[0].limits={100,1000,1000,10000};
    c.axes[0].profile=motion::Profile::create(10,c.axes[0].limits)->description();
    bool attempted{};
    progress=[&]{
        if(out[0]->status().state==hal::step::State::Completed && exec.active()){
            attempted=true;exec.service();
            EXPECT_NE(exec.status().state,io::motion::Executor::State::Faulted);
        }
    };
    ASSERT_TRUE(exec.submit(c));
    for(unsigned ms=0;ms<1000&&exec.active();++ms){hw->advance(10000);exec.renew(1);exec.service();}
    progress={};
    EXPECT_TRUE(attempted);
    EXPECT_EQ(exec.status().state,io::motion::Executor::State::Completed);
    EXPECT_EQ(exec.status().pulses[0],10U);
}

TEST_F(ExecutorTest, DetachedCompilationCannotArmOrClearAnotherOwnersOutput)
{
    gen->stop(); ASSERT_TRUE(gen->start()); hw->edges.clear();
    unsigned bound{};
    auto exec=std::make_unique<io::motion::Executor>(gen,
      [&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
          ++bound;return bool(out[i]->prepareSequence(std::move(s)));
      });
    io::motion::Executor::Command c{.id=1};
    c.axes[0].limits={100,1000,1000,10000};
    c.axes[0].profile=motion::Profile::create(10,c.axes[0].limits)->description();
    ASSERT_TRUE(exec->prepare(c,false));
    EXPECT_EQ(bound,0U);
    EXPECT_FALSE(exec->start(1));
    // Cancellation released the reservation; another owner can use the axis.
    ASSERT_TRUE(out[0]->prepareSequence(nominal));
    ASSERT_TRUE(out[0]->start());
    exec.reset();
    EXPECT_EQ(out[0]->status().state,hal::step::State::Running);
    out[0]->stop();
}
TEST_F(ExecutorTest, DetachedCompilationBindsOnlyAfterFinalAdmission)
{
    gen->stop(); ASSERT_TRUE(gen->start());
    io::motion::Executor exec(gen,[&](unsigned i,bool,std::shared_ptr<const hal::step::Sequence> s){
        return bool(out[i]->prepareSequence(std::move(s)));
    });
    io::motion::Executor::Command c{.id=1};
    c.axes[0].limits={100,1000,1000,10000};
    c.axes[0].profile=motion::Profile::create(10,c.axes[0].limits)->description();
    ASSERT_TRUE(exec.prepare(c,false));ASSERT_TRUE(exec.bind());ASSERT_TRUE(exec.start(1));
    for(unsigned ms=0;ms<1000&&exec.active();++ms){hw->advance(10000);exec.renew(1);exec.service();}
    EXPECT_EQ(exec.status().state,io::motion::Executor::State::Completed);
    EXPECT_EQ(exec.status().pulses[0],10U);
}
