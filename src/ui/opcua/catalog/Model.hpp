#pragma once
#include "Annotations.hpp"
#include "Call.hpp"
#include "Requests.hpp"
#include "ui/app/services/Types.hpp"
#include "common/app/State.hpp"

namespace ui::opcua::catalog
{
    struct System
    {
        [[= Method{"BeginControl", "Explicitly begin a new supervised control generation; local permission must be ON"}]]
        static std::uint64_t begin_control(Call& call) {return call.begin();}
        [[= Method{"Heartbeat", "Renew control every 100 ms; list held jobs, pad unused entries with zero", Access::Cleanup}]]
        static bool heartbeat(Call& call, std::uint64_t generation, std::array<std::uint64_t,8> held_jobs) {return call.heartbeat(generation, held_jobs);}
        [[= Method{"GetOperation", "Read the correlated outcome for this session", Access::ReadOnly}]]
        static Receipt operation(Call& call, std::uint64_t operation) {return call.result(operation);}
        [[= Method{"Initialize", "Initialize disabled drivers; never enables automatically"}]]
        static Receipt initialize(Call& call, std::uint64_t generation, std::uint64_t request) {return call.submit(generation, request, {.kind=services::Kind::Initialize});}
        [[= Method{"Enable", "Explicitly enable initialized drivers"}]]
        static Receipt enable(Call& call, std::uint64_t generation, std::uint64_t request) {return call.submit(generation, request, {.kind=services::Kind::Enable});}
        [[= Method{"Disable", "Abort and disable through the reserved urgent path"}]]
        static Receipt disable(Call& call, std::uint64_t generation, std::uint64_t request) {return call.submit(generation, request, {.kind=services::Kind::Disable});}
        [[= Method{"Stop", "Controlled axis/robot stop; raw motor stop keeps its existing abort semantics"}]]
        static Receipt stop(Call& call, std::uint64_t generation, std::uint64_t request) {return call.submit(generation, request, {.kind=services::Kind::Stop});}
        [[= Method{"Abort", "Immediate cessation; invalidate references as defined by the existing API"}]]
        static Receipt abort(Call& call, std::uint64_t generation, std::uint64_t request) {return call.submit(generation, request, {.kind=services::Kind::Abort});}
        [[= Variable{ "Simulation", "True for the native simulated plant", Unit::None, Quality::Local }]] bool simulation{};
        [[= Variable{ "UIState", "UI controller state", Unit::None, Quality::Local }]] app::State ui_state{};
        [[= Variable{ "IOState", "Last reported IO controller state" }]] app::State io_state{};
        [[= Variable{ "CombinedState", "Combined application state" }]] app::State combined_state{};
        [[= Variable{ "Connected", "ICC connection available", Unit::None, Quality::Local }]] bool connected{};
        [[= Variable{ "Initialized", "Initialization never enables motors" }]] bool initialized{};
        [[= Variable{ "Enabled", "Commanded shared motor enable, not proof of energization" }]] bool enabled{};
        [[= Variable{ "Readiness", "Actionable application readiness condition", Unit::None, Quality::Local }]] services::Text<160> readiness{};
        [[= Variable{ "Boot", "IO boot identity" }]] std::uint64_t boot{};
        [[= Variable{ "Configuration", "Mechanical configuration identity" }]] std::uint64_t configuration{};
        [[= Variable{ "SnapshotRevision", "Coherent telemetry revision" }]] std::uint64_t revision{};
        [[= Variable{ "Age", "Age of last telemetry receipt", Unit::Milliseconds, Quality::Local }]] std::uint64_t age{};
        [[= Variable{ "RemotePermission", "Local GUI/CLI permission; OFF after boot", Unit::None, Quality::Local }]] bool remote_permission{};
        [[= Variable{ "UTCValid", "Whether source timestamps have trustworthy UTC", Unit::None, Quality::Local }]] bool utc_valid{};
    };
    template<unsigned Index>
    struct Axis
    {
        [[= Method{"MoveRelative", "Relative axis move; native axis units and configured defaults"}]]
        static Receipt relative(Call& c, std::uint64_t generation, std::uint64_t request, AxisMove<Index> motion) {return c.submit(generation, request, motion.command(services::Kind::AxisRelative));}
        [[= Method{"MoveAbsolute", "Absolute axis move; requires reference"}]]
        static Receipt absolute(Call& c, std::uint64_t generation, std::uint64_t request, AxisMove<Index> motion) {return c.submit(generation, request, motion.command(services::Kind::AxisAbsolute));}
        [[= Method{"Jog", "Timed or supervised held axis jog"}]]
        static Receipt jog(Call& c, std::uint64_t generation, std::uint64_t request, AxisJog<Index> motion) {return c.submit(generation, request, motion.command());}
        [[= Method{"Home", "Existing axis reference procedure"}]]
        static Receipt home(Call& c, std::uint64_t generation, std::uint64_t request) {return c.submit(generation, request, {.kind=services::Kind::AxisHome, .axis=Index});}
        [[= Method{"Stop", "Controlled stop of axis or owning coordinated group"}]]
        static Receipt stop(Call& c, std::uint64_t generation, std::uint64_t request) {return c.submit(generation, request, {.kind=services::Kind::AxisStop, .axis=Index});}
        static constexpr Unit POSITION_UNIT{ Index == 2 ? Unit::Millimetres : Unit::Degrees };
        static constexpr Unit VELOCITY_UNIT{ Index == 2 ? Unit::MillimetresPerSecond : Unit::DegreesPerSecond };
        static constexpr Quality FEEDBACK_QUALITY{ Index == 0 ? Quality::Axis1Feedback : Index == 1 ? Quality::Axis2Feedback : Quality::Axis3Feedback };
        [[= Variable{ "CommandedPosition", "Commanded joint position", POSITION_UNIT, Quality::Configured }]] double commanded{};
        [[= Variable{ "FeedbackPosition", "Estimated feedback; see provenance and validity", POSITION_UNIT, FEEDBACK_QUALITY }]] double feedback{};
        [[= Variable{ "Velocity", "Reported velocity", VELOCITY_UNIT, Quality::Configured }]] double velocity{};
        [[= Variable{ "Minimum", "Configured lower soft limit", POSITION_UNIT, Quality::Configured }]] double minimum{};
        [[= Variable{ "Maximum", "Configured upper soft limit", POSITION_UNIT, Quality::Configured }]] double maximum{};
        [[= Variable{ "MaximumSpeed", "Configured velocity limit", VELOCITY_UNIT, Quality::Configured }]] double max_speed{};
        [[= Variable{ "Referenced", "Successful reference established" }]] bool referenced{};
        [[= Variable{ "FeedbackValid", "Feedback validity" }]] bool feedback_valid{};
        [[= Variable{ "EncoderFeedback", "Encoder if true; driver-phase estimate otherwise" }]] bool encoder{};
        [[= Variable{ "Owner", "Existing execution job ID, zero when idle" }]] std::uint64_t owner{};
    };
    struct Axes
    {
        [[= Object{ "Axis1", "First rotary axis" }]] Axis<0> axis1;
        [[= Object{ "Axis2", "Second rotary axis" }]] Axis<1> axis2;
        [[= Object{ "Axis3", "Linear axis" }]] Axis<2> axis3;
    };
    struct Robot
    {
        [[= Method{"Jog", "Existing Cartesian velocity jog; timed or supervised held motion"}]]
        static Receipt jog(Call& c, std::uint64_t generation, std::uint64_t request, CartesianJog motion) {return c.submit(generation, request, motion.command());}
        [[= Method{"Home", "Reference all axes in the configured sequence"}]]
        static Receipt home(Call& c, std::uint64_t generation, std::uint64_t request) {return c.submit(generation, request, {.kind=services::Kind::RobotHome});}
        [[= Method{"Stop", "Stop the owning robot job"}]]
        static Receipt stop(Call& c, std::uint64_t generation, std::uint64_t request) {return c.submit(generation, request, {.kind=services::Kind::Stop});}

        [[= Method{"MoveJointsRelative", "Existing application JointRelative operation; no automatic preemption"}]]
        static Receipt MoveJointsRelative(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::JointRelative));}
        [[= Method{"MoveJointsAbsolute", "Existing application JointAbsolute operation; no automatic preemption"}]]
        static Receipt MoveJointsAbsolute(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::JointAbsolute));}
        [[= Method{"MovePointRelative", "Existing application PointRelative operation; no automatic preemption"}]]
        static Receipt MovePointRelative(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::PointRelative));}
        [[= Method{"MovePointAbsolute", "Existing application PointAbsolute operation; no automatic preemption"}]]
        static Receipt MovePointAbsolute(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::PointAbsolute));}

        [[= Variable{ "Joints", "Estimated feedback joints: degrees, degrees, millimetres", Unit::None, Quality::RobotFeedback }]] std::array<double, 3> joints{};
        [[= Variable{ "EstimatedTCP", "Mixed encoder/driver-phase estimate; not fully measured TCP", Unit::Millimetres, Quality::RobotFeedback }]] std::array<double, 3> tcp{};
        [[= Variable{ "DerivedYaw", "Derived yaw, not an independent axis", Unit::Degrees, Quality::RobotFeedback }]] double yaw{};
        [[= Variable{ "FeedbackValid", "Configuration, reference and feedback are valid" }]] bool valid{};
        [[= Variable{ "Configured", "Geometry/workspace configuration is present" }]] bool configured{};
    };
    template<unsigned Index> struct Motor
    {
        [[= Method{"ReadDefaults", "Cached raw motor dynamics; no IO request", Access::ReadOnly}]]
        static MotorDefaults defaults(Call& call) {return call.motorDefaults(Index);}
        [[= Method{"MoveRelative", "Existing raw motor MotorRelative semantics; shaft units"}]]
        static Receipt MoveRelative(Call& c, std::uint64_t generation, std::uint64_t request, MotorMove motion) {return c.submit(generation, request, motion.command(services::Kind::MotorRelative, Index));}
        [[= Method{"MoveAbsolute", "Existing raw motor MotorAbsolute semantics; shaft units"}]]
        static Receipt MoveAbsolute(Call& c, std::uint64_t generation, std::uint64_t request, MotorMove motion) {return c.submit(generation, request, motion.command(services::Kind::MotorAbsolute, Index));}
        [[= Method{"Home", "Existing raw motor MotorHome semantics; shaft units"}]]
        static Receipt Home(Call& c, std::uint64_t generation, std::uint64_t request, MotorMove motion) {return c.submit(generation, request, motion.command(services::Kind::MotorHome, Index));}
        [[= Method{"SetSpeed", "Existing raw motor MotorSpeed semantics; shaft units"}]]
        static Receipt SetSpeed(Call& c, std::uint64_t generation, std::uint64_t request, MotorMove motion) {return c.submit(generation, request, motion.command(services::Kind::MotorSpeed, Index));}
        [[= Method{"SetDefaults", "Existing raw motor MotorDefaults semantics; shaft units"}]]
        static Receipt SetDefaults(Call& c, std::uint64_t generation, std::uint64_t request, MotorMove motion) {return c.submit(generation, request, motion.command(services::Kind::MotorDefaults, Index));}
        [[= Method{"Abort", "Immediate raw motor abort and existing reference invalidation"}]]
        static Receipt abort(Call& c, std::uint64_t generation, std::uint64_t request) {return c.submit(generation, request, {.kind=services::Kind::MotorAbort, .axis=Index});}
        static constexpr auto FEEDBACK_QUALITY{Index == 0 ? Quality::Motor1Feedback : Index == 1 ? Quality::Motor2Feedback : Quality::Motor3Feedback};
        [[= Variable{ "CommandedPosition", "Shaft degrees", Unit::Degrees }]] double commanded{};
        [[= Variable{ "FeedbackPosition", "Shaft feedback degrees; validity published separately", Unit::Degrees, FEEDBACK_QUALITY }]] double feedback{};
        [[= Variable{ "Velocity", "Shaft speed", Unit::Rpm }]] double velocity{};
        [[= Variable{ "Driver", "Configured driver type" }]] services::Text<32> driver{};
        [[= Variable{ "Ready", "Reported driver readiness" }]] bool ready{};
        [[= Variable{ "Fault", "Active or latched driver fault" }]] bool fault{};
        [[= Variable{ "FaultCode", "Typed driver fault code" }]] std::uint32_t fault_code{};
        [[= Variable{ "Warning", "Reported driver warning" }]] bool warning{};
        [[= Variable{ "ReferenceSwitch", "Reference/hard-limit input" }]] bool reference{};
        [[= Variable{ "FeedbackValid", "Feedback is valid" }]] bool valid{};
        [[= Variable{ "EncoderFeedback", "Encoder if true; driver-phase estimate otherwise" }]] bool encoder{};
        [[= Variable{ "Owner", "Existing job ID" }]] std::uint64_t owner{};
    };
    struct Motors
    {
        [[= Object{ "M1" }]] Motor<0> m1;
        [[= Object{ "M2" }]] Motor<1> m2;
        [[= Object{ "M3" }]] Motor<2> m3;
    };
    struct Capabilities
    {
        [[= Variable{ "ModelVersion", "Version of the frozen node-ID manifest", Unit::None, Quality::Local }]] services::Text<32> model_version{ '1', '.', '0', '.', '0' };
        [[= Variable{ "Programs", "Program execution is unavailable", Unit::None, Quality::Local }]] bool programs{};
        [[= Variable{ "Presets", "Saved-position management is unavailable", Unit::None, Quality::Local }]] bool presets{};
        [[= Variable{ "MinimumSamplingInterval", "Minimum supported sampling interval", Unit::Milliseconds, Quality::Local }]] std::uint32_t minimum_sampling{ 50 };
        [[= Variable{ "MaximumSessions", "Active session capacity", Unit::None, Quality::Local }]] std::uint32_t sessions{ 2 };
        [[= Variable{ "ResourceLimits", "Channels, subscriptions, items, pending commands, job history, page rows, chunk bytes, message bytes, heartbeat ms, expiry ms", Unit::None, Quality::Local }]]
        std::array<std::uint32_t,10> limits{4, 4, 64, 16, 32, 8, 8192, 16384, 100, 500};
    };
    struct IO
    {
        [[= Variable{"ReferenceSwitches", "Active reference/hard-limit inputs, ordered M1–M3"}]] std::array<bool,3> reference{};
        [[= Variable{"DriverFaults", "Reported/latched driver faults, ordered M1–M3"}]] std::array<bool,3> faults{};
        [[= Variable{"DriverWarnings", "Reported driver warnings, ordered M1–M3"}]] std::array<bool,3> warnings{};
        [[= Variable{"DriverReady", "Reported readiness, not proof of energization"}]] std::array<bool,3> ready{};
        [[= Variable{"EnableCommand", "Commanded shared enable; not independent power feedback"}]] bool enabled{};
    };
    struct Diagnostics
    {
        [[= Method{"ReadQueues", "Bounded ordinary/urgent application admission; full queues return explicit rejection", Access::ReadOnly}]]
        static FrontendQueues queues(Call& call) {return call.queues();}
        [[= Method{"ReadNetwork", "Cached network state and bounded allocator/packet-pool diagnostics; excludes credentials", Access::ReadOnly}]]
        static NetworkDiagnostics network(Call& call) {return call.network();}
        [[= Method{"RefreshMemory", "Queue an asynchronous UI/IO heap or stack snapshot; false means another query is pending", Access::ReadOnly}]]
        static bool refresh(Call& call, bool io, bool stacks) {return call.refreshDiagnostics(io, stacks);}
        [[= Method{"ReadGraphics", "Read cached GUI and renderer counters; no rendering work", Access::ReadOnly}]]
        static Graphics graphics(Call& call) {return call.graphics();}
        [[= Method{"ReadPage", "Cached UI/IO memory snapshot; no implicit IO query", Access::ReadOnly}]]
        static DiagnosticPage page(Call& call, std::uint32_t offset) {return call.diagnostics(offset);}
        [[= Variable{"OpcUaCapacity", "Reserved OPC UA arena capacity", Unit::Bytes, Quality::Local}]] std::uint64_t capacity{};
        [[= Variable{"OpcUaUsed", "Current arena allocation including allocator overhead", Unit::Bytes, Quality::Local}]] std::uint64_t used{};
        [[= Variable{"OpcUaPeak", "Peak arena allocation", Unit::Bytes, Quality::Local}]] std::uint64_t peak{};
        [[= Variable{"OpcUaAllocationFailures", "Failed arena allocation requests", Unit::None, Quality::Local}]] std::uint64_t failures{};
    };
    struct Jobs
    {
        [[= Method{"ReadPage", "At most eight existing execution jobs per page", Access::ReadOnly}]]
        static JobPage page(Call& call, std::uint32_t offset) {return call.jobs(offset, false);}
        [[= Method{"Cancel", "Cancel preparation or request the owning job's existing stop behavior"}]]
        static Receipt cancel(Call& call, std::uint64_t generation, std::uint64_t request, std::uint64_t job) {return call.submit(generation, request, {.kind=services::Kind::Cancel, .job=job});}
    };
    struct Planning
    {
        [[= Method{"PreviewJoints", "Existing application PreviewJoint operation; no automatic preemption"}]]
        static Receipt PreviewJoints(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::PreviewJoint));}
        [[= Method{"PreviewPoint", "Existing application PreviewPoint operation; no automatic preemption"}]]
        static Receipt PreviewPoint(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::PreviewPoint));}
        [[= Method{"PreviewLine", "Existing application PreviewLine operation; no automatic preemption"}]]
        static Receipt PreviewLine(Call& c, std::uint64_t generation, std::uint64_t request, RobotMove motion) {return c.submit(generation, request, motion.command(services::Kind::PreviewLine));}

        [[= Method{"ReadPage", "At most eight preview records; previews are not execution jobs", Access::ReadOnly}]]
        static JobPage page(Call& call, std::uint32_t offset) {return call.jobs(offset, true);}
        [[= Method{"ReadResult", "Preview endpoints and certified bounds; never executable, motor peaks in pulse units", Access::ReadOnly}]]
        static PreviewResult result(Call& call, std::uint64_t job) {return call.preview(job);}
        [[= Method{"Cancel", "Cancel an existing preview"}]]
        static Receipt cancel(Call& call, std::uint64_t generation, std::uint64_t request, std::uint64_t job) {return call.submit(generation, request, {.kind=services::Kind::CancelPreview, .job=job});}
    };
    struct Settings
    {
        [[= Method{"ReadConfiguration", "Cached mechanical configuration and calibration; unavailable when unconfigured", Access::ReadOnly}]]
        static Configuration configuration(Call& call) {return call.configuration();}
        [[= Method{"SetBrightness", "Enqueue display brightness 0–100 percent; readback reports applied value"}]]
        static Receipt brightness(Call& call, std::uint64_t generation, std::uint64_t request, std::uint32_t percent)
        {return call.submit(generation, request, {.kind=services::Kind::Brightness, .position={double(percent)}});}
        [[= Variable{"Brightness", "Applied backlight percentage", Unit::None, Quality::Local}]] std::uint32_t level{};
    };
    struct Messages
    {
        [[= Method{"ReadPage", "Latest eight shared application messages, newest first; times are monotonic", Access::ReadOnly}]]
        static MessagePage page(Call& call, std::uint32_t offset) {return call.messages(offset);}
        [[= Method{"Acknowledge", "Acknowledge history through sequence; never clears underlying hardware faults"}]]
        static Receipt acknowledge(Call& call, std::uint64_t generation, std::uint64_t request, std::uint64_t through_sequence)
        {return call.submit(generation, request, {.kind=services::Kind::AcknowledgeMessages, .job=through_sequence});}
        [[= Variable{"Revision", "Shared message history revision", Unit::None, Quality::Local}]] std::uint64_t revision{};
    };
    struct Model
    {
        [[= Object{ "System" }]] System system;
        [[= Object{ "Axes" }]] Axes axes;
        [[= Object{ "Robot" }]] Robot robot;
        [[= Object{ "Motors" }]] Motors motors;
        [[= Object{ "Planning" }]] Planning planning;
        [[= Object{ "Jobs" }]] Jobs jobs;
        [[= Object{ "IO" }]] IO io;
        [[= Object{ "Messages" }]] Messages messages;
        [[= Object{ "Settings" }]] Settings settings;
        [[= Object{ "Diagnostics" }]] Diagnostics diagnostics;
        [[= Object{ "Capabilities" }]] Capabilities capabilities;
        void update(const services::Snapshot& state);
    };
}
