#pragma once
#include "ControllerTypes.hpp"
#include "p2p/Bridge.hpp"

namespace control::protocol
{
    // The IDs, field order, units (SI), and little-endian primitives form protocol v1.
    // Requests are domain messages; p2p does not interpret their operation codes.
    enum class Operation : std::uint8_t
    {
        Snapshot,
        Drivers,
        Enable,
        Disable,
        Stop,
        Reset,
        InitializeDrivers,
        ConfigureDriver,
        ConfigureAxis,
        Defaults,
        SetDefaults,
        AxisDefaults,
        SetAxisDefaults,
        Velocity,
        AxisVelocity,
        Move,
        MoveAxis,
        Reference,
        ReferenceAxis,
        ReferenceAll,
        Trajectory
    };
    struct Request
    {
        Operation operation{};
        std::uint8_t motor{ 255 };
        p2p::Bytes data;
    };
    struct Response
    {
        bool success{ true };
        std::string error;
        p2p::Bytes data;
    };
    struct Progress
    {
        std::uint64_t execution_id{}, pulses{};
        double position{};
        bool referenced{};
    };
    struct Outcome
    {
        MotorResult result{ MotorResult::Faulted };
        std::string error;
        bool invalid_argument{};
    };
    struct State
    {
        ControllerTypes::PlanningSnapshot snapshot;
    };
    struct FaultState
    {
        std::array<hal::device::IStepperDriver::Status, 3> drivers;
        bool link_inhibited{};
    };
    inline constexpr p2p::EndpointId RequestEndpoint = 1, StopEndpoint = 2, ExecuteEndpoint = 3,
                                     StateEndpoint = 4, FaultEndpoint = 5;
    inline constexpr std::string_view RequestName = "motion.request", StopName = "motion.stop",
                                      ExecuteName = "motion.execute", StateName = "motion.state",
                                      FaultName = "motion.fault";
    void routes(p2p::Bridge&, bool io);
    void write(p2p::Writer&, const ControllerTypes::PlanningSnapshot&);
    ControllerTypes::PlanningSnapshot snapshot(p2p::Reader&);
    void write(p2p::Writer&, const std::array<ControllerTypes::DriverStatus, 3>&);
    std::array<ControllerTypes::DriverStatus, 3> drivers(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::Conversion&);
    ControllerTypes::Conversion conversion(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::DriverConfiguration&);
    ControllerTypes::DriverConfiguration driverConfiguration(p2p::Reader&);
    void write(p2p::Writer&, const MotionDefaults&);
    MotionDefaults defaults(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::AxisDefaults&);
    ControllerTypes::AxisDefaults axisDefaults(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::Move&);
    ControllerTypes::Move move(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::AxisMove&);
    ControllerTypes::AxisMove axisMove(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::AxisSpeed&);
    ControllerTypes::AxisSpeed speed(p2p::Reader&);
    void write(p2p::Writer&, const ControllerTypes::Trajectory&);
    ControllerTypes::Trajectory trajectory(p2p::Reader&);
}
namespace p2p
{
#define CONTROL_CODEC(T)                                                                                     \
    template<>                                                                                               \
    struct Codec<control::protocol::T>                                                                       \
    {                                                                                                        \
        static Bytes encode(const control::protocol::T&);                                                    \
        static control::protocol::T decode(std::span<const std::byte>);                                      \
    };
    CONTROL_CODEC(Request)
    CONTROL_CODEC(Response) CONTROL_CODEC(Progress) CONTROL_CODEC(Outcome) CONTROL_CODEC(State)
      CONTROL_CODEC(FaultState)
#undef CONTROL_CODEC
}
namespace pnm::utils::memory
{
// Local bus and wire use the same explicit codec; no double ABI-specific format.
#define CONTROL_SERIALIZATION(T)                                                                             \
    template<>                                                                                               \
    struct SerializationAdapter<control::protocol::T>                                                        \
    {                                                                                                        \
        static size_t bufferSize(const control::protocol::T& v) { return p2p::encode(v).size(); }            \
        static void serialize(const control::protocol::T& v, std::span<std::byte> out)                       \
        {                                                                                                    \
            auto b = p2p::encode(v);                                                                         \
            if (out.size() != b.size())                                                                      \
                throw std::invalid_argument("control payload size");                                         \
            std::ranges::copy(b, out.begin());                                                               \
        }                                                                                                    \
        static void deserialize(std::span<const std::byte> in, control::protocol::T& v)                      \
        {                                                                                                    \
            v = p2p::decode<control::protocol::T>(in);                                                       \
        }                                                                                                    \
    };
    CONTROL_SERIALIZATION(Request)
    CONTROL_SERIALIZATION(Response) CONTROL_SERIALIZATION(Progress) CONTROL_SERIALIZATION(Outcome)
      CONTROL_SERIALIZATION(State) CONTROL_SERIALIZATION(FaultState)
#undef CONTROL_SERIALIZATION
}
