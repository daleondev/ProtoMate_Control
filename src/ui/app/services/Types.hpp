#pragma once
#include "Text.hpp"
#include <array>
#include <cstdint>

namespace ui::services
{
    inline constexpr unsigned DIAGNOSTIC_ROWS{ 6 }, GRAPHICS_METRICS{ 8 }, INITIAL_GRAPHICS_VALUE{ 42 };
    inline constexpr double DEFAULT_LATCH_SPEED{ .5 };
    enum class Severity : std::uint8_t
    {
        Info,
        Warning,
        Error
    };
    struct QueueDiagnostics
    {
        unsigned pending{}, urgent{}, capacity{16}, urgent_capacity{4}, peak{};
        std::uint64_t rejected{};
    };
    enum class Kind : std::uint8_t
    {
        Initialize,
        Enable,
        Disable,
        Stop,
        Abort,
        Cancel,
        AxisRelative,
        AxisAbsolute,
        AxisHome,
        AxisJog,
        AxisStop,
        JointRelative,
        JointAbsolute,
        PointRelative,
        PointAbsolute,
        PointJog,
        RobotHome,
        PreviewJoint,
        PreviewPoint,
        PreviewLine,
        CancelPreview,
        MotorRelative,
        MotorAbsolute,
        MotorHome,
        MotorSpeed,
        MotorDefaults,
        MotorAbort,
        MemoryUI,
        MemoryIO,
        MemoryUIStack,
        MemoryIOStack,
        Brightness,
        AcknowledgeMessages
    };
    // Presentation boundary. Degrees/mm, rpm for raw motors; the adapter owns
    // conversion to unit-safe application values. No wire or HAL objects here.
    struct Command
    {
        Kind kind{};
        unsigned axis{};
        std::array<double, 3> position{};
        double speed{}, acceleration{}, deceleration{}, jerk{}, timeout{}, duration{ 1 },
          latch{ DEFAULT_LATCH_SPEED };
        unsigned buffer{};
        bool continuous{}, confirmed{};
        std::uint64_t job{};
    };
    struct Outcome
    {
        std::uint64_t operation{}, job{};
        bool accepted{};
        Text<text_capacity::MESSAGE> message{};
        Severity severity{ Severity::Info };
        std::uint64_t sequence{};
        enum class Phase : std::uint8_t { Submitted, Completed, Rejected, Unknown };
        Phase phase{ Phase::Submitted };
    };
    struct Axis
    {
        double commanded{}, feedback{}, velocity{}, minimum{}, maximum{}, max_speed{};
        double shaft{}, rpm{}, shaft_feedback{}, acceleration{}, deceleration{}, jerk{};
        unsigned fault_code{};
        bool referenced{}, valid{}, velocity_valid{}, reference{}, ready{}, fault{}, warning{}, encoder{};
        std::uint64_t owner{};
        Text<text_capacity::SHORT> driver{};
    };
    struct Snapshot
    {
        std::array<Axis, 3> axes{};
        std::array<unsigned, 3> joint_for_motor{ 0, 1, 2 };
        const Axis& motor(unsigned index) const { return axes[joint_for_motor[index]]; }
        unsigned ui_state{}, io_state{}, combined_state{};
        bool io_state_fresh{};
        std::array<double, 3> commanded{}, feedback{}, tcp{}, feedback_tcp{}; // SI poses, TCP mm
        double yaw{}, feedback_yaw{};
        bool simulation{}, connected{}, failed{}, fresh{}, configured{}, robot_configured{}, initialized{},
          enabled{}, pose_valid{}, feedback_valid{};
        unsigned family{};
        std::uint64_t sequence{}, boot{}, configuration{}, age_ms{}, received_us{};
        Text<text_capacity::MESSAGE> condition{};
        Text<text_capacity::DETAIL> details{};
        Outcome outcome{};
    };
    struct Job
    {
        std::uint64_t id{}, busy{}, source{};
        unsigned state{}, family{}, axes{};
        bool terminal{}, preview{};
        double seconds{};
        std::array<double, 3> endpoint{};
        Text<text_capacity::NAME> status{};
        Text<text_capacity::DETAIL> detail{};
        Severity severity{ Severity::Info };
    };
    struct DetailedJob : Job
    {
        std::array<double, 3> requested{};
        Text<text_capacity::SHORT> requested_space{}, requested_units{};
        bool relative{};
    };
    struct DiagnosticRow
    {
        Text<text_capacity::NAME> name{};
        std::array<std::uint64_t, 4> values{};
    };
    struct DiagnosticPage
    {
        std::array<DiagnosticRow, DIAGNOSTIC_ROWS> rows{};
        unsigned count{}, total{}, omitted{}, allocator{};
        bool available{}, busy{}, io{}, stacks{}, shared{};
        std::uint64_t age_ms{}, capacity{}, used{};
        Text<text_capacity::LABEL> error{};
    };
    struct Message
    {
        Text<text_capacity::MESSAGE> text{};
        Severity severity{ Severity::Info };
        std::uint64_t sequence{}, time_ms{}, operation{};
        bool acknowledged{};
    };
    struct Graphics
    {
        bool available{};
        unsigned brightness{};
        std::uint64_t frames{}, presented{}, underruns{}, dma_errors{}, render_failures{},
          queue_overflow{}, maximum_render_us{}, scene_presented{}, scene_latency_us{};
    };
    struct Configuration
    {
        bool available{}, calibrated{}, simulation{}, kinematics{}, neutral_available{};
        std::uint64_t identity{}, hardware{};
        std::array<std::uint32_t,3> motor_for_axis{}, home_order{};
        std::array<bool,3> opposite{}, home_configured{}, home_positive{};
        std::array<double,3> reduction{}, reference{}, minimum{}, maximum{}, velocity{}, acceleration{},
          deceleration{}, jerk{}, motor_step_degrees{}, neutral{}, base_mm{}, tool_mm{}, work_mm{},
          workspace_min_mm{}, workspace_max_mm{}, home_seek_rpm{}, home_latch_rpm{}, home_timeout_s{};
        std::array<double,2> links_mm{};
        double screw_lead_mm{}, base_yaw_deg{}, tool_yaw_deg{}, work_yaw_deg{}, approximation_mm{};
    };
    struct NetworkDiagnostics
    {
        bool wifi{}, connected{}, busy{}, saved{};
        std::uint32_t state{}, address{};
        std::int32_t error{};
        std::uint64_t capacity{}, used{}, peak{}, failures{}, packets{}, available_packets{},
          empty_packet_requests{}, packet_suspensions{}, invalid_packet_releases{};
    };
}
