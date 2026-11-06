#include "common/motion/VisualGeometry.hpp"
#include "ui/gui/hmi/Presenter.hpp"
#include <gtest/gtest.h>
#include <numbers>
using namespace ui::gui::hmi;
namespace
{
    class BackendFixture : public Backend
    {
      public:
        Command last{};
        unsigned submissions{}, releases{}, holds{};
        void read(Snapshot&) override {}
        Outcome submit(const Command& c) override
        {
            last = c;
            ++submissions;
            return { .job = 17, .accepted = true };
        }
        unsigned jobs(unsigned, std::span<Job>, bool) override { return 0; }
        Outcome hold(const Command& c, int, int, int, int) override
        {
            last = c;
            ++holds;
            return { .job = 18, .accepted = true };
        }
        void input(const Input&) override {}
        void release() override { ++releases; }
    };
}
TEST(Hmi, KeypadEditingAndCancellationNeverSubmit)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    p.act(Field0, true);
    p.act(Key0 + 1, true);
    p.act(KeySign, true);
    EXPECT_EQ(b.submissions, 0U);
    p.act(KeyCancel, true);
    EXPECT_DOUBLE_EQ(m.values[0], 0);
    EXPECT_FALSE(m.keypad);
    p.act(Field0, true);
    std::snprintf(m.number.data(), m.number.size(), "-2.5");
    p.act(KeyApply, true);
    EXPECT_DOUBLE_EQ(m.values[0], -2.5);
    EXPECT_EQ(b.submissions, 0U);
}
TEST(Hmi, RejectsInvalidNumericFactorsAndNonfiniteValues)
{
    Model m;
    Presenter p{ m };
    m.page = Page::Robot;
    p.act(Field0 + 10, true);
    for (const auto* invalid : { "nan", "inf", "2", "-1", "" }) {
        std::snprintf(m.number.data(), m.number.size(), "%s", invalid);
        p.act(KeyApply, true);
        EXPECT_TRUE(m.keypad);
        EXPECT_DOUBLE_EQ(m.values[10], .25);
    }
    std::snprintf(m.number.data(), m.number.size(), ".5");
    p.act(KeyApply, true);
    EXPECT_FALSE(m.keypad);
    EXPECT_DOUBLE_EQ(m.values[10], .5);
}
TEST(Hmi, HeldJogUsesTenPercentAndReleaseDoesNotWaitForAcceptance)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    m.page = Page::Axes;
    m.continuous = true;
    m.state.axes[0].maxSpeed = 40;
    Presenter p{ m };
    p.act(JogMinus, true);
    EXPECT_EQ(b.holds, 1U);
    EXPECT_DOUBLE_EQ(b.last.speed, -4);
    EXPECT_TRUE(b.last.continuous);
    p.act(JogMinus, false);
    EXPECT_EQ(b.releases, 1U);
    p.navigate(Page::Home);
    EXPECT_EQ(b.releases, 2U);
}
TEST(Hmi, RawAbortingReplacementNeedsConfirmation)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    m.page = Page::Motors;
    m.state.axes[0].owner = 8;
    Presenter p{ m };
    p.act(Execute, true);
    EXPECT_TRUE(m.confirmation);
    EXPECT_EQ(b.submissions, 0U);
    p.act(Confirm, true);
    EXPECT_EQ(b.submissions, 1U);
    EXPECT_TRUE(b.last.confirmed);
}
TEST(Hmi, CartesianButtonsAreFinitePointDestinations)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    m.page = Page::Cartesian;
    m.step = 1;
    Presenter p{ m };
    p.act(YMinus, true);
    EXPECT_EQ(b.last.kind, Kind::PointRelative);
    EXPECT_DOUBLE_EQ(b.last.position[1], -1);
    EXPECT_DOUBLE_EQ(b.last.position[0], 0);
    EXPECT_FALSE(b.last.continuous);
    EXPECT_DOUBLE_EQ(b.last.speed, .25);
}
TEST(Hmi, ProgramsAndPresetsGenerateNoCommands)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    p.navigate(Page::Programs);
    p.navigate(Page::Presets);
    EXPECT_EQ(b.submissions, 0U);
}
TEST(VisualRegistration, PreservesCadLocalOrigins)
{
    const auto a = ::motion::visual::relative(0, .3), b = ::motion::visual::relative(1, -.2),
               c = ::motion::visual::relative(2, .04);
    EXPECT_DOUBLE_EQ(a.translation[2], .052);
    EXPECT_DOUBLE_EQ(a.yaw, .3);
    EXPECT_NEAR(b.translation[2], .3179, 1e-12);
    EXPECT_NEAR(c.translation[0], .3584, 1e-12);
    EXPECT_NEAR(
      a.translation[2] + b.translation[2] + c.translation[2] + ::motion::visual::TOOL_Z, .0689, 1e-12);
}
TEST(Hmi, PreviewSelectionNeverSubmitsAndLineNeverExecutes)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    p.navigate(Page::Preview);
    p.act(PreviewLine, true);
    EXPECT_EQ(b.submissions, 0U);
    EXPECT_EQ(m.tab, 2U);
    p.act(RunPreview, true);
    EXPECT_EQ(b.last.kind, Kind::PreviewLine);
    EXPECT_EQ(b.submissions, 1U);
    p.act(PreviewPoint, true);
    EXPECT_EQ(b.submissions, 1U);
    p.act(RunPreview, true);
    EXPECT_EQ(b.last.kind, Kind::PreviewPoint);
}
TEST(Hmi, RobotUnitsFactorsAndRawBufferAreExplicit)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    p.navigate(Page::Robot);
    m.values[0] = 30;
    m.values[1] = -45;
    m.values[2] = 12;
    p.act(Execute, true);
    EXPECT_EQ(b.last.kind, Kind::JointRelative);
    EXPECT_DOUBLE_EQ(b.last.position[1], -45);
    EXPECT_DOUBLE_EQ(b.last.speed, .25);
    EXPECT_DOUBLE_EQ(b.last.acceleration, 1);
    p.act(PreviewPoint, true);
    p.act(Absolute, true);
    p.act(Execute, true);
    EXPECT_EQ(b.last.kind, Kind::PointAbsolute);
    p.navigate(Page::Motors);
    m.buffer = 4;
    p.act(Execute, true);
    EXPECT_EQ(b.last.buffer, 4U);
    EXPECT_EQ(b.last.kind, Kind::MotorAbsolute);
}
TEST(Hmi, MotorMetadataRemainsInMotorOrderWithPermutedAxes)
{
    Snapshot s;
    s.jointForMotor = { 2, 0, 1 };
    s.axes[2].owner = 99;
    s.axes[0].owner = 7;
    EXPECT_EQ(s.motor(0).owner, 99U);
    EXPECT_EQ(s.motor(1).owner, 7U);
}

TEST(Hmi, SupportedControlsUseTheSharedBackendOperations)
{
    struct Case
    {
        Page page;
        unsigned action;
        Kind kind;
    };
    constexpr Case cases[]{ { Page::Home, Initialize, Kind::Initialize },
                            { Page::Home, Enable, Kind::Enable },
                            { Page::Home, Disable, Kind::Disable },
                            { Page::Home, Reference, Kind::RobotHome },
                            { Page::Axes, Reference, Kind::AxisHome },
                            { Page::Axes, AxisStop, Kind::AxisStop },
                            { Page::Motors, Reference, Kind::MotorHome },
                            { Page::Motors, Abort, Kind::MotorAbort },
                            { Page::Motors, ActiveSpeed, Kind::MotorSpeed },
                            { Page::Motors, ApplyDefaults, Kind::MotorDefaults },
                            { Page::Settings, MemoryUI, Kind::MemoryUI },
                            { Page::Settings, MemoryIO, Kind::MemoryIO },
                            { Page::Settings, MemoryUIStack, Kind::MemoryUIStack },
                            { Page::Settings, MemoryIOStack, Kind::MemoryIOStack } };
    for (const auto& c : cases) {
        Model m;
        BackendFixture b;
        m.backend = &b;
        m.page = c.page;
        m.axis = 2;
        Presenter p{ m };
        p.act(c.action, true);
        EXPECT_EQ(b.submissions, 1U);
        EXPECT_EQ(b.last.kind, c.kind);
        EXPECT_EQ(b.last.axis, 2U);
    }
}

TEST(Hmi, StopRemainsAvailableDuringKeypadConfirmationAndEmptyPages)
{
    for (const auto page :
         { Page::Home, Page::Axes, Page::Robot, Page::Motors, Page::Programs, Page::Presets }) {
        Model m;
        BackendFixture b;
        m.backend = &b;
        m.page = page;
        m.keypad = true;
        m.confirmation = true;
        Presenter p{ m };
        p.act(GlobalStop, true);
        EXPECT_EQ(b.last.kind, Kind::Stop);
        EXPECT_EQ(b.submissions, 1U);
        EXPECT_EQ(b.releases, 1U);
    }
}

TEST(Hmi, AllCartesianStepDirectionsAndJobCancellationHaveExplicitIdentity)
{
    for (unsigned i = 0; i < 6; ++i) {
        Model m;
        BackendFixture b;
        m.backend = &b;
        m.page = Page::Cartesian;
        m.step = .1;
        Presenter p{ m };
        p.act(XMinus + i, true);
        EXPECT_EQ(b.last.kind, Kind::PointRelative);
        for (unsigned axis = 0; axis < 3; ++axis)
            EXPECT_DOUBLE_EQ(b.last.position[axis], axis == i / 2 ? (i % 2 ? .1 : -.1) : 0);
    }
    Model m;
    BackendFixture b;
    m.backend = &b;
    m.page = Page::Jobs;
    m.jobCount = 2;
    m.jobs[1].id = 123;
    Presenter p{ m };
    p.act(Cancel1, true);
    EXPECT_EQ(b.last.kind, Kind::Cancel);
    EXPECT_EQ(b.last.job, 123U);
    m.previews = true;
    p.act(Cancel1, true);
    EXPECT_EQ(b.last.kind, Kind::CancelPreview);
    EXPECT_EQ(b.last.job, 123U);
}
