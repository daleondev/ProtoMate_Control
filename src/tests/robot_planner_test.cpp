#include "ui/motion/Planner.hpp"
#include "common/motion/Trajectory.hpp"
#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>

namespace
{
    namespace tr=motion::trajectory;
    using namespace pnm::units;
    using namespace pnm::units::literals;
    tr::Capabilities hardware()
    {
        tr::Capabilities c;
        c.step_radians.fill(std::numbers::pi/1600);
        c.reference_steps.fill(1200);
        c.limits.fill({-1e8,1e8,99999,1e6,1e6,1e7});
        c.coordinated_execution=true;
        c.hardware_fingerprint=tr::fingerprint(c);
        return c;
    }
    struct Fixture
    {
        ui::motion::RobotModel model{ui::motion::simulationConfiguration(hardware()),hardware()};
        std::unique_ptr<ui::motion::Planner::Workspace> arena=std::make_unique<ui::motion::Planner::Workspace>();
        ui::motion::Planner planner{model,*arena};
        tr::Counts origin{1200,1200,1200};
    };
}

TEST(RobotPolynomial, HermitePreservesSixBoundaryConditions)
{
    const motion::KinematicState a{12,3,-2},b{-8,-4,5};
    const auto p=motion::hermite(a,b,2.5);
    EXPECT_DOUBLE_EQ(p.at(0),a.position); EXPECT_NEAR(p.at(1),b.position,1e-12);
    EXPECT_NEAR(p.at(0,1)/2.5,a.velocity,1e-12); EXPECT_NEAR(p.at(1,1)/2.5,b.velocity,1e-12);
    EXPECT_NEAR(p.at(0,2)/6.25,a.acceleration,1e-12); EXPECT_NEAR(p.at(1,2)/6.25,b.acceleration,1e-12);
}
TEST(RobotPolynomial, BernsteinBoundsCoverInteriorOvershootAndRestriction)
{
    const motion::Polynomial p{{0,4,-4}};
    const auto bounds=p.range(); EXPECT_LE(bounds.minimum,0); EXPECT_GE(bounds.maximum,1);
    const auto part=p.restrict(0.25,0.75);
    for(unsigned i=0;i<=1000;++i) {
        const double u=i/1000.0;
        EXPECT_NEAR(part.at(u),p.at(0.25+u*0.5),1e-12);
        EXPECT_GE(p.at(u),bounds.minimum); EXPECT_LE(p.at(u),bounds.maximum);
    }
}
TEST(RobotPolynomial, RootsIncludeTurningTangency)
{
    const motion::Polynomial p{{0.1875,-1,1}};
    std::array<double,5> roots{};
    ASSERT_EQ(p.roots(roots),2); EXPECT_NEAR(roots[0],0.25,1e-12); EXPECT_NEAR(roots[1],0.75,1e-12);
    const motion::Polynomial tangent{{0.25,-1,1}};
    ASSERT_EQ(tangent.roots(roots),1); EXPECT_NEAR(roots[0],0.5,1e-12);
}
TEST(RobotWire, MaximumUploadFitsTransportAndRoundTripsNegativeCoordinates)
{
    tr::Request request;
    request.operation=tr::Operation::Upload; request.boot=0x0102030405060708ULL;
    request.expected_position={-17,400,0}; request.count=4;
    for(auto& s:request.segments) { s.ticks=12345;s.motors[0].coefficients={-1,2,3,4,5,6};s.path_end=1; }
    const auto packet=tr::encode(request);
    EXPECT_LT(packet.size+16+64,2048);
    EXPECT_EQ(std::to_integer<unsigned>(packet.bytes[4]),8U);
    const auto decoded=tr::decodeRequest(packet); ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->boot,request.boot);EXPECT_EQ(decoded->expected_position,request.expected_position);
    EXPECT_EQ(decoded->segments[3].motors[0].coefficients,request.segments[3].motors[0].coefficients);
    const auto encoded=tr::encode(*decoded);
    EXPECT_EQ(encoded.size,packet.size);EXPECT_EQ(encoded.bytes,packet.bytes);
}
TEST(RobotWire, CorruptionTruncationAndTrailingBytesAreRejected)
{
    auto p=tr::encode(tr::Request{});
    p.bytes[5]^=std::byte{0x80}; EXPECT_FALSE(tr::decodeRequest(p));
    p=tr::encode(tr::Request{});--p.size;EXPECT_FALSE(tr::decodeRequest(p));
    p=tr::encode(tr::Request{});++p.size;EXPECT_FALSE(tr::decodeRequest(p));
    p.size=65535;EXPECT_FALSE(tr::decodeRequest(p));
}
TEST(RobotWire, ReplyAndExplicitSerializationAdapterRoundTrip)
{
    tr::Reply reply;reply.capabilities=hardware();reply.position={-12,34,56};reply.referenced=7;
    reply.boundary[0]={-12,3,4};
    const auto packet=tr::encode(reply);
    std::array<std::byte,tr::Packet::CAPACITY> buffer{};
    auto out=std::span(buffer).first(packet.size);
    pnm::utils::memory::SerializationAdapter<tr::Packet>::serialize(packet,out);
    tr::Packet restored;
    pnm::utils::memory::SerializationAdapter<tr::Packet>::deserialize(out,restored);
    const auto decoded=tr::decodeReply(restored);ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->position,reply.position);EXPECT_DOUBLE_EQ(decoded->boundary[0].velocity,3);
    EXPECT_EQ(decoded->capabilities.hardware_fingerprint,tr::fingerprint(decoded->capabilities));
}
TEST(RobotTrajectory, LimitsIncludeInteriorExtremaAndAllFiniteFields)
{
    tr::Segment s{.ticks=10'000'000};s.path_end=1;
    s.motors[0].coefficients={0,4,-4};
    auto limits=hardware().limits;limits[0].maximum=0.9;
    EXPECT_EQ(tr::validate(s,limits),tr::Error::PositionLimit);
    limits=hardware().limits; EXPECT_EQ(tr::validate(s,limits),tr::Error::None);
    s.motors[1].coefficients[3]=std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(tr::validate(s,limits),tr::Error::InvalidInput);
}
TEST(RobotTrajectory, BrakingIsMonotoneContinuousAndStopsAtFiniteDistance)
{
    for(double acceleration:{-1.0,0.0,1.0}) {
        const auto p=motion::Profile::brake(2,acceleration,{5,3,3,10});ASSERT_TRUE(p);
        EXPECT_NEAR(p->at(0).velocity,2,1e-12);EXPECT_NEAR(p->at(0).acceleration,acceleration,1e-12);
        EXPECT_NEAR(p->at(p->duration()).velocity,0,1e-12);
        double previous=0;
        for(unsigned i=0;i<=1000;++i) { const auto s=p->at(p->duration()*i/1000.0);EXPECT_GE(s.position+1e-12,previous);previous=s.position; }
        const auto copy=motion::Profile::fromDescription(p->description());ASSERT_TRUE(copy);
        EXPECT_DOUBLE_EQ(copy->distance(),p->distance());
    }
    EXPECT_FALSE(motion::Profile::brake(0.001,-3,{5,3,3,10}));
}
TEST(RobotConfiguration, NoPhysicalDefaultAndSimulationCannotEnableMotion)
{
    EXPECT_FALSE(ui::motion::boardRobotConfiguration());
    Fixture f;EXPECT_EQ(f.model.physicalAdmission(),tr::Error::Configuration);
    auto c=ui::motion::simulationConfiguration(hardware());c.expected_hardware^=1;
    EXPECT_THROW((ui::motion::RobotModel{c,hardware()}),std::invalid_argument);
}
TEST(RobotConfiguration, FingerprintCoversHomingAndFeedAndRejectsInvalidHardwareLimits)
{
    auto c=ui::motion::simulationConfiguration(hardware());
    const auto original=ui::motion::RobotModel{c,hardware()}.fingerprint();
    c.homing[1].positive=!c.homing[1].positive;
    EXPECT_NE(original,(ui::motion::RobotModel{c,hardware()}.fingerprint()));
    c=ui::motion::simulationConfiguration(hardware());c.feed=10_mm_s;
    EXPECT_NE(original,(ui::motion::RobotModel{c,hardware()}.fingerprint()));
    auto h=hardware();h.limits[0].jerk=std::numeric_limits<double>::quiet_NaN();
    h.hardware_fingerprint=tr::fingerprint(h);c.expected_hardware=h.hardware_fingerprint;
    EXPECT_THROW((ui::motion::RobotModel{c,h}),std::invalid_argument);
}
TEST(RobotConfiguration, MotorMappingAndReversedReductionRoundTrip)
{
    auto c=ui::motion::simulationConfiguration(hardware());c.motor_for_joint={2,0,1};
    c.rotary[1].direction=ui::motion::AxisDirection::OppositeToMotor;
    ui::motion::RobotModel model{c,hardware()};
    const ui::motion::JointPosition p{720_deg,60_deg,42_mm};
    const auto q=model.toJoints(model.toMotors(p));
    EXPECT_NEAR(q.shoulder.get(),p.shoulder.get(),1e-12);EXPECT_NEAR(q.elbow.get(),p.elbow.get(),1e-12);EXPECT_NEAR(q.z.get(),p.z.get(),1e-12);
}
TEST(RobotPlanner, JointMoveProducesContinuousBoundedMotorProfiles)
{
    Fixture f;
    ui::motion::Move command{.joints={10_deg,80_deg,40_mm},.job=7};
    ASSERT_EQ(f.planner.prepare(command,f.origin),tr::Error::None);
    const auto plan=f.planner.compile();ASSERT_TRUE(plan) << tr::name(plan.error());
    EXPECT_GT(plan->segments,0U);EXPECT_GT(plan->duration,0);
    auto previous=f.planner.segments().front().at(0);
    for(const auto& s:f.planner.segments()) {
        EXPECT_EQ(tr::validate(s,f.model.motorLimits()),tr::Error::None);
        EXPECT_TRUE(tr::continuous(previous,s.at(0)));previous=s.at(1);
    }
    EXPECT_TRUE(tr::atRest(previous));
    auto target=f.model.toMotors(command.joints);
    for(unsigned i=0;i<3;++i) { EXPECT_EQ(plan->endpoint[i],*tr::quantize(target[i])); }
}
TEST(RobotPlanner, CartesianLineKeepsTcpWithinDeclaredError)
{
    Fixture f;
    const auto start=f.model.kinematics().forward(f.model.toJoints({1200,1200,1200}));ASSERT_TRUE(start);
    auto target=start->position;target.x-=10_mm;target.y+=10_mm;target.z+=5_mm;
    ui::motion::Move command{.kind=ui::motion::MoveKind::Linear,.target=target,.job=8};
    ASSERT_EQ(f.planner.prepare(command,f.origin),tr::Error::None);
    const auto plan=f.planner.compile();ASSERT_TRUE(plan) << tr::name(plan.error());
    double time=0;
    for(const auto& s:f.planner.segments()) {
        for(unsigned k=0;k<=50;++k) {
            const double u=k/50.0;const auto motor=s.at(u);tr::Positions q{};
            for(unsigned i=0;i<3;++i) { q[i]=motor[i].position; }
            const auto pose=f.model.kinematics().forward(f.model.toJoints(q));ASSERT_TRUE(pose);
            const double progress=f.planner.pathAt(time+u*s.ticks*tr::TICK_SECONDS).position;
            const double dx=pose->position.x.get()-(start->position.x.get()+progress*(target.x-start->position.x).get());
            const double dy=pose->position.y.get()-(start->position.y.get()+progress*(target.y-start->position.y).get());
            const double dz=pose->position.z.get()-(start->position.z.get()+progress*(target.z-start->position.z).get());
            EXPECT_LE(std::sqrt(dx*dx+dy*dy+dz*dz),f.model.configuration().path_tolerance.get());
        }
        time+=s.ticks*tr::TICK_SECONDS;
    }
}
TEST(RobotPlanner, RejectsUnsafeInteriorEvenWhenEndpointsAreReachable)
{
    Fixture f;
    const auto q=f.model.toJoints({1200,1200,1200});
    auto pose=f.model.kinematics().forward(q)->position;pose.x=-pose.x;pose.y=-pose.y;
    const auto error=f.planner.prepare({.kind=ui::motion::MoveKind::Linear,.target=pose},f.origin);
    EXPECT_NE(error,tr::Error::None);
}
TEST(RobotPlanner, RejectsResolutionBelowToleranceAndInvalidOverride)
{
    auto c=ui::motion::simulationConfiguration(hardware());c.path_tolerance=0.001_mm;
    ui::motion::RobotModel model{c,hardware()};
    auto arena=std::make_unique<ui::motion::Planner::Workspace>();
    ui::motion::Planner planner{model,*arena};
    EXPECT_EQ(planner.prepare({.joints={10_deg,80_deg,40_mm}},{1200,1200,1200}),tr::Error::PathTolerance);
    Fixture f;ASSERT_EQ(f.planner.prepare({.joints={10_deg,80_deg,40_mm}},f.origin),tr::Error::None);
    EXPECT_FALSE(f.planner.compile({},1.1)); EXPECT_FALSE(f.planner.compile({},0));
}
TEST(RobotPlanner, StoppingContinuationPreservesBoundaryAndReservesTravel)
{
    Fixture f;
    ASSERT_EQ(f.planner.prepare({.joints={30_deg,100_deg,50_mm}},f.origin),tr::Error::None);
    const auto original=f.planner.compile();ASSERT_TRUE(original);
    const auto start=f.planner.pathAt(original->duration*0.35);
    const auto stopping=f.planner.compile(start,1,true);
    ASSERT_TRUE(stopping) << tr::name(stopping.error());
    EXPECT_GT(stopping->final_progress,start.position);EXPECT_LE(stopping->final_progress,1);
    EXPECT_TRUE(tr::atRest(f.planner.segments().back().at(1)));
    EXPECT_NEAR(f.planner.pathAt(0).velocity,start.velocity,1e-10);
    EXPECT_NEAR(f.planner.pathAt(0).acceleration,start.acceleration,1e-10);
    const auto impossible=f.planner.compile({0.9999,1,0},1,true);
    EXPECT_FALSE(impossible);EXPECT_EQ(impossible.error(),tr::Error::NoStoppingPath);
}
TEST(RobotPlanner, RetimingCartesianCurveKeepsOriginalGeometry)
{
    Fixture f;
    const auto start=f.model.kinematics().forward(f.model.toJoints({1200,1200,1200}));ASSERT_TRUE(start);
    ui::motion::Move command{.kind=ui::motion::MoveKind::CartesianCurve};
    command.curve[0]={{start->position.x.get(),-0.01}};
    command.curve[1]={{start->position.y.get(),0.01,0.02,-0.02}};
    command.curve[2]={{start->position.z.get(),0.005}};
    ASSERT_EQ(f.planner.prepare(command,f.origin),tr::Error::None);
    const auto plan=f.planner.compile();ASSERT_TRUE(plan) << tr::name(plan.error());
    double time=0;
    for(const auto& s:f.planner.segments()) {
        for(unsigned k=0;k<=100;++k) {
            const double u=k/100.0;tr::Positions motor{};
            for(unsigned i=0;i<3;++i) { motor[i]=s.motors[i].at(u); }
            const auto tcp=f.model.kinematics().forward(f.model.toJoints(motor));ASSERT_TRUE(tcp);
            const auto progress=f.planner.pathAt(time+u*s.ticks*tr::TICK_SECONDS).position;
            const double dx=tcp->position.x.get()-command.curve[0].at(progress);
            const double dy=tcp->position.y.get()-command.curve[1].at(progress);
            const double dz=tcp->position.z.get()-command.curve[2].at(progress);
            EXPECT_LE(std::sqrt(dx*dx+dy*dy+dz*dz),plan->path_error_bound);
        }
        time+=s.ticks*tr::TICK_SECONDS;
    }
}
