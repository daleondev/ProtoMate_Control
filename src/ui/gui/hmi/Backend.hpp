#pragma once
#include "Layout.hpp"
#include "Text.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace ui::gui::hmi
{
    inline constexpr unsigned DIAGNOSTIC_ROWS{ 6 }, GRAPHICS_METRICS{ 8 }, INITIAL_GRAPHICS_VALUE{ 42 };
    inline constexpr double DEFAULT_LATCH_SPEED{ .5 };
    enum class Severity : std::uint8_t
    {
        Info,
        Warning,
        Error
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
        MemoryIOStack
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
        std::uint64_t id{}, busy{};
        unsigned state{}, family{}, axes{};
        bool terminal{}, preview{};
        double seconds{};
        std::array<double, 3> endpoint{};
        Text<text_capacity::NAME> status{};
        Text<text_capacity::DETAIL> detail{};
        Severity severity{ Severity::Info };
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
    struct Input
    {
        bool down{}, valid{};
        int x{}, y{};
        std::chrono::steady_clock::time_point sampled{};
    };
    class Backend
    {
      public:
        Backend() = default;
        Backend(const Backend&) = delete;
        auto operator=(const Backend&) -> Backend& = delete;
        Backend(Backend&&) = delete;
        auto operator=(Backend&&) -> Backend& = delete;

        virtual ~Backend() = default;
        virtual void read(Snapshot& result) = 0;
        virtual Outcome submit(const Command& command) = 0;
        virtual unsigned jobs(unsigned offset, std::span<Job> output, bool previews) = 0;
        // Begin from the GUI, supervise from the acquisition/client workers.
        virtual Outcome hold(const Command& command, layout::Bounds bounds) = 0;
        virtual void input(const Input& input) = 0;
        virtual void release() = 0;
        virtual void diagnostics(unsigned /*unused*/, DiagnosticPage& result) { result = {}; }
    };
}
