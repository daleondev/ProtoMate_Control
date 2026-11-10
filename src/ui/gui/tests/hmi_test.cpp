#include "common/motion/VisualGeometry.hpp"
#include "ui/gui/hmi/NumericField.hpp"
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
        Outcome hold(const Command& c, ui::gui::hmi::layout::Bounds) override
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
    EXPECT_EQ(m.jog_mode, 2U);
    m.state.axes[0].max_speed = 40;
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
TEST(Hmi, CartesianHoldSubmitsDirectionIndependentOfStepSize)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    m.page = Page::Cartesian;
    m.cartesian_step = 10;
    Presenter p{ m };
    p.act(YMinus, true);
    EXPECT_EQ(b.last.kind, Kind::PointJog);
    EXPECT_DOUBLE_EQ(b.last.position[1], -1);
    EXPECT_DOUBLE_EQ(b.last.position[0], 0);
    EXPECT_TRUE(b.last.continuous);
    EXPECT_DOUBLE_EQ(b.last.speed, .25);
    EXPECT_EQ(b.holds, 1U);
    EXPECT_EQ(b.submissions, 0U);
    p.act(YMinus, false);
    EXPECT_EQ(b.releases, 1U);
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
    EXPECT_EQ(m.coordinates, 2U);
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
    s.joint_for_motor = { 2, 0, 1 };
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
        Action action;
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
        m.cartesian_step = .1;
        Presenter p{ m };
        p.act(StepMode, true);
        p.act(XMinus + i, true);
        EXPECT_EQ(b.last.kind, Kind::PointRelative);
        for (unsigned axis = 0; axis < 3; ++axis) {
            EXPECT_DOUBLE_EQ(b.last.position[axis], axis == i / 2 ? (i % 2 ? .1 : -.1) : 0);
        }
    }
    Model m;
    BackendFixture b;
    m.backend = &b;
    m.page = Page::Jobs;
    m.job_count = 2;
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

#include "ui/gui/hmi/Gestures.hpp"
TEST(Gestures, PinchIsRelativeBoundedAndRequiresRelease)
{
    Gestures g;
    hal::device::TouchReport r{};
    r.points[0] = { true, 300, 250 };
    EXPECT_FALSE(g.update(r, true, true).captured);
    r.points[1] = { true, 400, 250 };
    auto v = g.update(r, true, true);
    EXPECT_TRUE(v.captured);
    EXPECT_EQ(v.scale, 1);
    r.points[1].x = 500;
    v = g.update(r, true, true);
    EXPECT_NEAR(v.scale, .5f, 1e-6);
    r.points[1].active = false;
    EXPECT_TRUE(g.update(r, true, true).captured);
    EXPECT_TRUE(g.update(r, true, true).captured);
    r.points[0].active = false;
    EXPECT_FALSE(g.update(r, true, true).captured);
    r.points[0].active = true;
    EXPECT_FALSE(g.update(r, true, true).captured);
}
TEST(Gestures, ContactBegunOutsideCannotBecomeAPinch)
{
    Gestures g;
    hal::device::TouchReport r{};
    r.points[0] = { true, 40, 200 };
    g.update(r, true, true);
    r.points[0].x = 300;
    r.points[1] = { true, 400, 200 };
    EXPECT_TRUE(g.update(r, true, true).captured);
    r.points[1].x = 500;
    EXPECT_EQ(g.update(r, true, true).scale, 1);
}
TEST(Gestures, NearZeroJitterAndSlotReorderingDoNotJump)
{
    Gestures g;
    hal::device::TouchReport r{};
    r.points[0] = { true, 300, 250 };
    r.points[1] = { true, 301, 250 };
    EXPECT_EQ(g.update(r, true, true).scale, 1);
    r.points[1].x = 400;
    EXPECT_EQ(g.update(r, true, true).scale, 1);
    r.points[1].x = 401;
    EXPECT_EQ(g.update(r, true, true).scale, 1);
    std::swap(r.points[0], r.points[1]);
    EXPECT_EQ(g.update(r, true, true).scale, 1);
    r.cancelled = true;
    EXPECT_TRUE(g.update(r, true, true).captured);
}
TEST(Gestures, WheelOnlyZoomsVisibleViewportAndHonorsFocus)
{
    Gestures g;
    hal::device::TouchReport r{};
    r.pointer_x = 300;
    r.pointer_y = 300;
    r.wheel = .5f;
    EXPECT_NEAR(g.update(r, true, true).scale, std::exp(-.06f), 1e-6);
    r.wheel = -1;
    EXPECT_GT(g.update(r, true, true).scale, 1);
    r.pointer_x = 800;
    EXPECT_EQ(g.update(r, true, true).scale, 1);
    r.pointer_x = 300;
    EXPECT_EQ(g.update(r, false, true).scale, 1);
    EXPECT_EQ(g.update(r, true, false).scale, 1);
}
TEST(Hmi, ChangingModesAndTabsCancelsHeldInput)
{
    for (Action action : { AxisJogPage,
                           AxisPosition,
                           AxisDynamics,
                           StepMode,
                           Timed,
                           Continuous,
                           Axis1,
                           Axis2,
                           Axis3,
                           More,
                           Advanced }) {
        Model m;
        BackendFixture backend;
        m.backend = &backend;
        Presenter p{ m };
        m.page = Page::Axes;
        p.act(action, true);
        EXPECT_EQ(backend.submissions, 0U);
        EXPECT_GT(backend.releases, 0U);
    }
}

#include "ui/gui/hmi/Annotations.hpp"
TEST(Gestures, OverlayControlsNeverStartPinchOrWheelZoom)
{
    ui::gui::hmi::Gestures gesture;
    hal::device::TouchReport report{};
    const ui::gui::hmi::Gestures::Exclusion overlay{ 200, 200, 100, 56 };
    report.pointer_x = 220;
    report.pointer_y = 220;
    report.wheel = 1;
    EXPECT_EQ(gesture.update(report, true, true, { &overlay, 1 }).scale, 1);
    report.points[0] = { .active = true, .x = 220, .y = 220 };
    report.points[1] = { .active = true, .x = 350, .y = 220 };
    EXPECT_TRUE(gesture.update(report, true, true, { &overlay, 1 }).captured);
    report.points[1].x = 450;
    EXPECT_EQ(gesture.update(report, true, true, { &overlay, 1 }).scale, 1);
}
TEST(Annotations, CrowdedAnchorsStayInsideAndDoNotOverlap)
{
    using namespace ui::gui;
    for (int x : { 0, 320, 639 }) {
        for (int y : { 0, 200, 399 }) {
            std::array<scene3d::ProjectedAnchor, 3> anchors{};
            for (auto& a : anchors) {
                a = { int16_t(x), int16_t(y), 12.5f, true };
            }
            const auto boxes = hmi::layout_badges(anchors);
            for (unsigned i = 0; i < 3; ++i) {
                ASSERT_TRUE(anchors[i].visible);
                EXPECT_GE(boxes[i].x, 0);
                EXPECT_GE(boxes[i].y, 56); // below the scene's top controls
                EXPECT_LE(boxes[i].right(), 640);
                EXPECT_LE(boxes[i].bottom(), 356); // above Reset View
                for (unsigned j = 0; j < i; ++j) {
                    EXPECT_FALSE(boxes[i].intersect(boxes[j]));
                }
            }
        }
    }
}
#include "ui/gui/util/WheelQueue.hpp"
TEST(Gestures, WheelQueueKeepsCoordinatesAndConsumesEachDeltaOnce)
{
    ui::gui::util::WheelQueue q;
    using namespace std::chrono_literals;
    const auto now = ui::gui::util::WheelQueue::Clock::now();
    hal::device::TouchReport r{};
    r.wheel = .25;
    r.pointer_x = 900;
    r.pointer_y = 300;
    ASSERT_TRUE(q.push(r, now));
    r.pointer_x = 400;
    r.wheel = -.5;
    ASSERT_TRUE(q.push(r, now));
    q.consume(r, now + 1ms);
    EXPECT_EQ(r.pointer_x, 900);
    EXPECT_EQ(r.wheel, .25);
    q.consume(r, now + 2ms);
    EXPECT_EQ(r.pointer_x, 400);
    EXPECT_EQ(r.wheel, -.5);
    q.consume(r, now + 3ms);
    EXPECT_EQ(r.wheel, 0);
    r.wheel = 1;
    ASSERT_TRUE(q.push(r, now));
    q.consume(r, now + 101ms);
    EXPECT_EQ(r.wheel, 0);
    r.wheel = 1;
    for (unsigned i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.push(r, now));
    }
    EXPECT_FALSE(q.push(r, now));
    EXPECT_EQ(q.dropped(), 1);
    q.consume(r, now);
    EXPECT_EQ(r.wheel, 0);
    r.wheel = 1;
    ASSERT_TRUE(q.push(r, now));
    r.cancelled = true;
    ASSERT_TRUE(q.push(r, now));
    q.consume(r, now);
    EXPECT_EQ(r.wheel, 0);
}

TEST(Hmi, CartesianTimedJogUsesDurationAndFactorsWithoutInputSupervision)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    p.navigate(Page::Cartesian);
    p.act(Timed, true);
    m.cartesian_step = 10;
    m.values[8] = 2.5;
    m.values[10] = .2;
    m.values[4] = .5;
    p.act(ZPlus, true);
    EXPECT_EQ(b.last.kind, Kind::PointJog);
    EXPECT_DOUBLE_EQ(b.last.position[2], 1);
    EXPECT_DOUBLE_EQ(b.last.duration, 2.5);
    EXPECT_DOUBLE_EQ(b.last.speed, .2);
    EXPECT_DOUBLE_EQ(b.last.acceleration, .5);
    EXPECT_DOUBLE_EQ(b.last.deceleration, 1);
    EXPECT_FALSE(b.last.continuous);
    EXPECT_EQ(b.holds, 0U);
    EXPECT_EQ(b.submissions, 1U);
    EXPECT_EQ(m.jog_mode, 2U);
    p.act(StepSmall, true);
    EXPECT_DOUBLE_EQ(m.cartesian_step, .1);
    EXPECT_DOUBLE_EQ(m.step, 1);
}
TEST(Hmi, CartesianMoveToUsesPointCoordinatesAndSettings)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    p.navigate(Page::Cartesian);
    p.act(CartesianDynamics, true);
    EXPECT_EQ(m.cartesian_tab, 2U);
    p.act(Field0 + 4, true);
    std::snprintf(m.number.data(), m.number.size(), "2");
    p.act(KeyApply, true);
    EXPECT_TRUE(m.keypad);
    p.act(KeyCancel, true);
    p.act(CartesianPosition, true);
    EXPECT_EQ(m.cartesian_tab, 1U);
    m.values[0] = -25;
    m.values[1] = 200;
    m.values[2] = 15;
    m.values[5] = .6;
    m.values[7] = 20;
    p.act(Absolute, true);
    p.act(Execute, true);
    EXPECT_EQ(b.last.kind, Kind::PointAbsolute);
    EXPECT_EQ(b.last.position, (std::array<double, 3>{ -25, 200, 15 }));
    EXPECT_DOUBLE_EQ(b.last.speed, .25);
    EXPECT_DOUBLE_EQ(b.last.deceleration, .6);
    EXPECT_DOUBLE_EQ(b.last.timeout, 20);
    p.act(Relative, true);
    p.act(Execute, true);
    EXPECT_EQ(b.last.kind, Kind::PointRelative);
    p.act(CartesianJogPage, true);
    EXPECT_EQ(m.cartesian_tab, 0U);
    EXPECT_GE(b.releases, 4U);
}

TEST(Hmi, NumericEditorStartsEmptyAndCancelPreservesTheForm)
{
    Model m;
    Presenter p{ m };
    m.values[0] = 42.5;
    p.act(Field0, true);
    EXPECT_STREQ(m.number.data(), "");
    const auto session = m.editor_revision;
    p.key('1');
    p.key('2');
    p.key('\x1b');
    EXPECT_FALSE(m.keypad);
    EXPECT_DOUBLE_EQ(m.values[0], 42.5);
    p.act(Field0, true);
    EXPECT_NE(m.editor_revision, session);
    EXPECT_STREQ(m.number.data(), "");
    p.key('\n');
    EXPECT_TRUE(m.keypad);
    EXPECT_DOUBLE_EQ(m.values[0], 42.5);
}
TEST(Hmi, KeyboardUsesTheSameRulesAndNeverSubmitsMotion)
{
    Model m;
    BackendFixture b;
    m.backend = &b;
    Presenter p{ m };
    m.page = Page::Robot;
    p.act(Field0 + 10, true);
    p.key('-');
    p.key('0');
    p.key(',');
    p.key('2');
    p.key('5');
    EXPECT_STREQ(m.number.data(), "0.25");
    p.key('\b');
    p.key('3');
    p.key('\n');
    EXPECT_FALSE(m.keypad);
    EXPECT_DOUBLE_EQ(m.values[10], .23);
    p.key('9');
    p.key('\n'); // closed modal ignores further input
    EXPECT_DOUBLE_EQ(m.values[10], .23);
    EXPECT_EQ(b.submissions, 0U);
    p.act(Field0, true);
    for (char k : std::string_view{ "-12.5" }) {
        p.key(k);
    }
    p.key('-'); // minus sets sign, it does not toggle a typed negative back to positive
    EXPECT_STREQ(m.number.data(), "-12.5");
    p.key('\x7f');
    EXPECT_STREQ(m.number.data(), "");
    for (char k : std::string_view{ "-2.5" }) {
        p.key(k);
    }
    p.key('\n');
    EXPECT_DOUBLE_EQ(m.values[0], -2.5);
}
TEST(Hmi, FieldValidationChecksNumericSyntaxLimitsAndDefaults)
{
    Model m;
    m.page = Page::Axes;
    m.state.configured = m.state.fresh = true;
    m.state.axes[0] = { .commanded = 20, .minimum = -100, .maximum = 100, .max_speed = 30 };
    m.field = 0;
    auto r = NumericField::forModel(m);
    EXPECT_TRUE(r.parse("80"));
    EXPECT_FALSE(r.parse("80.01"));
    EXPECT_TRUE(r.parse("-120"));
    EXPECT_FALSE(r.parse("-120.01"));
    m.absolute = true;
    r = NumericField::forModel(m);
    EXPECT_TRUE(r.parse("100"));
    EXPECT_FALSE(r.parse("101"));
    for (const auto* text : { "", "-", ".", "-.", "nan", "inf", "0x10", "1e2", " 1", "1.2.3" }) {
        EXPECT_FALSE(r.parse(text)) << text;
    }
    m.page = Page::Preview;
    m.absolute = false; // previews are destinations regardless of the move-page selection
    r = NumericField::forModel(m);
    EXPECT_TRUE(r.parse("100"));
    EXPECT_FALSE(r.parse("-101"));
    m.page = Page::Axes;
    m.field = 3;
    r = NumericField::forModel(m);
    EXPECT_TRUE(r.parse("0"));
    EXPECT_TRUE(r.parse("30"));
    EXPECT_FALSE(r.parse("31"));
    EXPECT_FALSE(r.parse("-1"));
    for (unsigned f : { 4U, 5U, 6U }) {
        m.field = f;
        r = NumericField::forModel(m);
        EXPECT_TRUE(r.parse("0"));
        EXPECT_FALSE(r.parse("-1"));
        m.page = Page::Robot;
        r = NumericField::forModel(m);
        EXPECT_TRUE(r.parse("1"));
        EXPECT_FALSE(r.parse("1.01"));
        m.page = Page::Axes;
    }
    m.field = 8;
    r = NumericField::forModel(m);
    EXPECT_FALSE(r.parse("0"));
    EXPECT_TRUE(r.parse(".1"));
    EXPECT_FALSE(r.parse("1000001"));
    m.field = 11;
    r = NumericField::forModel(m);
    EXPECT_TRUE(r.parse("0"));
    EXPECT_TRUE(r.parse("100"));
    EXPECT_FALSE(r.parse("100.01"));
    m.page = Page::Motors;
    m.motor_tab = 1;
    m.values[3] = 10;
    m.values[9] = .5;
    m.field = 3;
    r = NumericField::forModel(m);
    EXPECT_FALSE(r.parse("0"));
    EXPECT_FALSE(r.parse("-1"));
    EXPECT_FALSE(r.parse(".5"));
    EXPECT_TRUE(r.parse("1"));
    m.field = 9;
    r = NumericField::forModel(m);
    EXPECT_FALSE(r.parse("0"));
    EXPECT_FALSE(r.parse("10"));
    EXPECT_TRUE(r.parse(".5"));
    m.field = 7;
    r = NumericField::forModel(m);
    EXPECT_FALSE(r.parse("0"));
    EXPECT_TRUE(r.parse("10"));
    m.motor_tab = 2;
    for (unsigned f : { 4U, 5U }) {
        m.field = f;
        r = NumericField::forModel(m);
        EXPECT_FALSE(r.parse("0"));
        EXPECT_TRUE(r.parse("1"));
    }
}
TEST(Hmi, MessageSeverityAndInfoFilterSurviveRingWrap)
{
    Model m;
    Presenter p{ m };
    for (unsigned i = 0; i < 10; ++i) {
        char text[32];
        std::snprintf(text, sizeof(text), "Info %u", i);
        m.report(text);
    }
    m.report("Busy", Severity::Warning);
    m.report("Fault", Severity::Error);
    m.report("Ready");
    EXPECT_EQ(m.visibleMessage(0)->severity, Severity::Info);
    p.act(ToggleInfo, true);
    ASSERT_NE(m.visibleMessage(0), nullptr);
    EXPECT_STREQ(m.visibleMessage(0)->text.data(), "Fault");
    EXPECT_EQ(m.visibleMessage(0)->severity, Severity::Error);
    EXPECT_STREQ(m.visibleMessage(1)->text.data(), "Busy");
    EXPECT_EQ(m.visibleMessage(2), nullptr);
    p.navigate(Page::Home);
    p.navigate(Page::Alarms);
    EXPECT_FALSE(m.show_info);
    p.act(ToggleInfo, true);
    EXPECT_STREQ(m.visibleMessage(0)->text.data(), "Ready");
    EXPECT_NE(m.visibleMessage(7), nullptr);
    EXPECT_EQ(m.visibleMessage(8), nullptr);
    m.state.failed = true;
    p.act(Acknowledge, true);
    EXPECT_TRUE(m.state.failed);
    EXPECT_EQ(m.message_count, 1U);
}

TEST(HmiText, BoundedFormattingTerminatesAndPreservesNumericPresentation)
{
    std::array<char, 64> text{};
    ui::gui::hmi::format(text, "{:.1f} ° / {:.2f} mm / {}", -12.25, 0.125, 18446744073709551615ULL);
    EXPECT_STREQ(text.data(), "-12.2 ° / 0.12 mm / 18446744073709551615");
    std::array<char, 4> small{};
    ui::gui::hmi::format(small, "{}", "abcdef");
    EXPECT_STREQ(small.data(), "abc");
    std::array<char, 1> terminator{ 'x' };
    ui::gui::hmi::format(terminator, "{}", 42);
    EXPECT_EQ(terminator[0], '\0');
    ui::gui::hmi::format(std::span<char>{}, "{}", 42);
}
