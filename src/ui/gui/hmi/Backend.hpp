#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace ui::gui::hmi
{
    template<unsigned N>
    using Text = std::array<char, N>;
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
        double speed{}, acceleration{}, deceleration{}, jerk{}, timeout{}, duration{ 1 }, latch{ .5 };
        unsigned buffer{};
        bool continuous{}, confirmed{};
        std::uint64_t job{};
    };
    struct Outcome
    {
        std::uint64_t operation{}, job{};
        bool accepted{};
        Text<160> message{};
        Severity severity{ Severity::Info };
    };
    struct Axis
    {
        double commanded{}, feedback{}, velocity{}, minimum{}, maximum{}, maxSpeed{};
        double shaft{}, rpm{}, shaftFeedback{}, acceleration{}, deceleration{}, jerk{};
        unsigned faultCode{};
        bool referenced{}, valid{}, velocityValid{}, reference{}, ready{}, fault{}, warning{}, encoder{};
        std::uint64_t owner{};
        Text<32> driver{};
    };
    struct Snapshot
    {
        std::array<Axis, 3> axes{};
        std::array<unsigned, 3> jointForMotor{ 0, 1, 2 };
        const Axis& motor(unsigned index) const { return axes[jointForMotor[index]]; }
        unsigned uiState{}, ioState{}, combinedState{};
        bool ioStateFresh{};
        std::array<double, 3> commanded{}, feedback{}, tcp{}, feedbackTcp{}; // SI poses, TCP mm
        double yaw{}, feedbackYaw{};
        bool simulation{}, connected{}, failed{}, fresh{}, configured{}, robotConfigured{}, initialized{},
          enabled{}, poseValid{}, feedbackValid{};
        unsigned family{};
        std::uint64_t sequence{}, boot{}, configuration{}, ageMs{}, receivedUs{};
        Text<160> condition{};
        Text<192> details{};
        Outcome outcome{};
    };
    struct Job
    {
        std::uint64_t id{}, busy{};
        unsigned state{}, family{}, axes{};
        bool terminal{}, preview{};
        double seconds{};
        std::array<double, 3> endpoint{};
        Text<40> status{};
        Text<192> detail{};
        Severity severity{ Severity::Info };
    };
    struct DiagnosticRow
    {
        Text<40> name{};
        std::array<std::uint64_t, 4> values{};
    };
    struct DiagnosticPage
    {
        std::array<DiagnosticRow, 6> rows{};
        unsigned count{}, total{}, omitted{}, allocator{};
        bool available{}, busy{}, io{}, stacks{}, shared{};
        std::uint64_t ageMs{}, capacity{}, used{};
        Text<96> error{};
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
        virtual ~Backend() = default;
        virtual void read(Snapshot& result) = 0;
        virtual Outcome submit(const Command& command) = 0;
        virtual unsigned jobs(unsigned offset, std::span<Job> output, bool previews) = 0;
        // Begin from the GUI, supervise from the acquisition/client workers.
        virtual Outcome hold(const Command& command, int x, int y, int width, int height) = 0;
        virtual void input(const Input& input) = 0;
        virtual void release() = 0;
        virtual void diagnostics(unsigned, DiagnosticPage& result) { result = {}; }
    };
}
