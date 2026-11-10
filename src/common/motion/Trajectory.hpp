#pragma once

#include "Motor.hpp"
#include "OperatingEnvelope.hpp"
#include "Polynomial.hpp"
#include "Profile.hpp"
#include "ProgressPath.hpp"
#include <array>
#include <cstdint>
#include <expected>
#include <pneumo/common.hpp>
#include <span>
#include <string_view>

namespace motion::trajectory
{
    inline constexpr std::uint16_t VERSION{ 4 };
    inline constexpr std::uint32_t TICK_NS{ 100 };
    inline constexpr std::uint64_t CHECKPOINT_TICKS{ operating::CHECKPOINT_TICKS };
    inline constexpr std::uint64_t COMMIT_HORIZON_TICKS{ operating::COMMIT_TICKS };
    inline constexpr std::uint64_t LOOKAHEAD_TICKS{ 5'000'000 };
    inline constexpr std::uint64_t LEASE_TICKS{ 2'500'000 };
    inline constexpr std::size_t UPLOAD_SEGMENTS{ 4 };
    inline constexpr std::size_t IO_SEGMENTS{ 256 };
    inline constexpr std::size_t UI_SEGMENTS{ 512 };
    inline constexpr std::size_t COMMANDS{ 64 };
    inline constexpr std::size_t IO_SEGMENT_BYTES{ 64UZ * 1024 };
    inline constexpr std::uint32_t MINIMUM_PERIOD_TICKS{ 100 };
    inline constexpr std::uint32_t HIGH_TICKS{ 50 };
    inline constexpr std::uint32_t DIRECTION_MARGIN_TICKS{ 50 };
    inline constexpr double MAX_POSITION{ 1e9 };
    inline constexpr double TICK_SECONDS{ TICK_NS * 1e-9 };

    enum class Error : std::uint8_t
    {
        None,
        InvalidInput,
        Configuration,
        Unreferenced,
        Busy,
        Stale,
        Capacity,
        Discontinuity,
        PositionLimit,
        VelocityLimit,
        AccelerationLimit,
        JerkLimit,
        Timing,
        Protocol,
        Checksum,
        Order,
        Unsupported,
        Fault,
        Unknown,
        Unreachable,
        Singularity,
        Workspace,
        PathTolerance,
        SubdivisionLimit,
        LeaseExpired,
        Starved,
        Cancelled,
        TooLate,
        NoStoppingPath,
        ExecutionDeadline,
        StopSource,
        StopBoundary,
        StopState,
        StopDuration,
        StopCrossing,
        StopOutsidePath,
        StopScaling,
        ScheduleInUse,
        HomingTimeout,
        SpliceDeadline,
        SpliceRejected,
        ReferenceActive
    };
    std::string_view name(Error error) noexcept;
    enum class State : std::uint8_t
    {
        Disabled,
        Unreferenced,
        Ready,
        Staged,
        Running,
        Homing,
        Holding,
        Held,
        Completed,
        Stopped,
        Faulted,
        Unknown
    };
    enum class Operation : std::uint8_t
    {
        Inspect,
        Acquire,
        Release,
        Upload,
        Commit,
        Hold,
        Resume,
        Stop,
        Abort,
        Disable,
        Renew,
        Initialize,
        Enable,
        Home,
        Prepare
    };
    enum class Branch : std::uint8_t
    {
        Continuation,
        Stopping
    };

    using MotorStates = std::array<KinematicState, 3>;
    using Positions = std::array<double, 3>;
    using Counts = std::array<std::int64_t, 3>;

    struct Limits
    {
        double minimum{}, maximum{}, velocity{}, acceleration{}, deceleration{}, jerk{};
    };
    struct Segment
    {
        std::uint64_t ticks{};
        std::array<Polynomial, 3> motors{};
        std::uint64_t job{};
        double path_begin{}, path_end{};
        [[nodiscard]] MotorStates at(double u) const noexcept;
    };
    static_assert(sizeof(Segment) * IO_SEGMENTS <= IO_SEGMENT_BYTES);

    struct Capabilities
    {
        std::uint16_t version{ VERSION };
        std::uint32_t tick_ns{ TICK_NS };
        std::array<double, 3> step_radians{};
        std::array<double, 3> reference_steps{};
        std::array<Limits, 3> limits{};
        std::uint64_t hardware_fingerprint{};
        std::uint32_t minimum_period_ticks{ MINIMUM_PERIOD_TICKS }, high_ticks{ HIGH_TICKS },
          direction_setup_ticks{ DIRECTION_MARGIN_TICKS }, direction_hold_ticks{ DIRECTION_MARGIN_TICKS };
        std::uint16_t capacity{ IO_SEGMENTS };
        bool coordinated_execution{};
        std::uint8_t reference_forward_mask{
            ALL_AXES_MASK
        }; // motor-coordinate homing direction, bits M1..M3
    };
    // One normalized, rest-to-rest progress law. Motor displacements are
    // quantized once on UI; IO validates them against the reservation snapshot.
    enum class ProgramKind : std::uint8_t
    {
        Finite,
        Home,
        Jog,
        CartesianJog
    };
    struct HomeStage
    {
        double seek_rpm{}, latch_rpm{}, timeout{};
    };
    struct FiniteProgram
    {
        Profile::Description progress{};
        Profile::Limits progress_limits{};
        Counts displacement{};
        std::array<Limits, 3> limits{};
        double timeout{};
        ProgramKind kind{};
        std::array<HomeStage, 3> home{};
        std::array<std::uint8_t, 3> home_order{ 0, 1, 2 };
        bool home_to_neutral{};
        Counts home_neutral{};
        std::array<ProgressPath, 3> path{};
    };
    struct Run
    {
        std::uint64_t boot{}, session{}, job{};
    };
    struct Update
    {
        std::uint64_t job{};
        JobState state{};
        Error error{};
        Counts position{};
        bool counts_exact{ true };
    };
    struct Request
    {
        Operation operation{ Operation::Inspect };
        std::uint16_t version{ VERSION };
        std::uint64_t boot{}, session{}, generation{}, configuration{}, revision{}, splice_tick{};
        Counts expected_position{};
        std::uint32_t index{};
        std::uint8_t count{};
        Branch branch{ Branch::Continuation };
        bool final{};
        std::array<Segment, UPLOAD_SEGMENTS> segments{};
        Family family{};
        std::uint8_t axes{};
        std::uint64_t job{};
        std::array<std::uint64_t, 3> axis_revisions{};
        // Renew batches include all active jobs, including raw buffered jobs.
        std::array<std::uint64_t, MOTOR_JOB_CAPACITY> renew_jobs{};
        FiniteProgram program{}; // Encoded only for Prepare.
    };
    struct Reply
    {
        Error error{ Error::None };
        State state{ State::Disabled };
        Capabilities capabilities{};
        std::uint64_t boot{}, session{}, generation{}, revision{}, tick{}, splice_tick{}, job{};
        std::uint32_t next_index{}, stopping_index{};
        std::uint16_t credits{};
        std::uint8_t referenced{};
        bool enabled{}, counts_exact{ true };
        Counts position{};
        MotorStates boundary{};
        double path{};
        Family family{};
        std::array<std::uint64_t, 3> owners{}, axis_revisions{};
        // Lease acknowledgements do not repeat the full mechanical snapshot.
        bool acknowledgement{};
    };

    // One explicit, versioned little-endian codec for all inter-core trajectory
    // messages. Unused native padding and unused segment slots never cross ICC.
    struct Packet
    {
        static constexpr std::size_t CAPACITY{ 1536 };
        std::uint16_t size{};
        std::array<std::byte, CAPACITY> bytes{};
    };
    [[nodiscard]] Packet encode(const Request& request);
    [[nodiscard]] Packet encode(const Reply& reply);
    [[nodiscard]] std::expected<Request, Error> decode_request(const Packet& packet) noexcept;
    [[nodiscard]] std::expected<Reply, Error> decode_reply(const Packet& packet) noexcept;
    [[nodiscard]] std::uint32_t checksum(std::span<const std::byte> data) noexcept;
    [[nodiscard]] std::uint64_t fingerprint(const Capabilities& capabilities) noexcept;
    [[nodiscard]] Error validate(const Segment& segment, const std::array<Limits, 3>& limits) noexcept;
    [[nodiscard]] bool continuous(const MotorStates& a, const MotorStates& b) noexcept;
    [[nodiscard]] bool at_rest(const MotorStates& state) noexcept;
    [[nodiscard]] std::expected<std::int64_t, Error> quantize(double position) noexcept;
}

namespace pnm::utils::memory
{
    template<>
    struct SerializationAdapter<motion::trajectory::Packet>
    {
        static std::size_t bufferSize(const motion::trajectory::Packet& p);
        static void serialize(const motion::trajectory::Packet& p, std::span<std::byte> output);
        static void deserialize(std::span<const std::byte> input, motion::trajectory::Packet& p);
    };
}
