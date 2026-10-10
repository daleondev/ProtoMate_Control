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
    inline constexpr std::uint8_t ALL_MOTORS{ 255 };
    inline constexpr std::size_t MAX_ERROR_TEXT{ 255 };

    struct Request
    {
        Operation operation{};
        std::uint8_t motor{ ALL_MOTORS };
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
    inline constexpr p2p::EndpointId REQUEST_ENDPOINT = 1, STOP_ENDPOINT = 2, EXECUTE_ENDPOINT = 3,
                                     STATE_ENDPOINT = 4, FAULT_ENDPOINT = 5;
    inline constexpr std::string_view REQUEST_NAME = "motion.request", STOP_NAME = "motion.stop",
                                      EXECUTE_NAME = "motion.execute", STATE_NAME = "motion.state",
                                      FAULT_NAME = "motion.fault";
    void routes(p2p::Bridge& b, bool io);
    void write(p2p::Writer& w, const ControllerTypes::PlanningSnapshot& s);
    ControllerTypes::PlanningSnapshot snapshot(p2p::Reader& r);
    void write(p2p::Writer& w, const std::array<ControllerTypes::DriverStatus, 3>& values);
    std::array<ControllerTypes::DriverStatus, 3> drivers(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::Conversion& c);
    ControllerTypes::Conversion conversion(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::DriverConfiguration& c);
    ControllerTypes::DriverConfiguration driver_configuration(p2p::Reader& r);
    void write(p2p::Writer& w, const MotionDefaults& d);
    MotionDefaults defaults(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::AxisDefaults& d);
    ControllerTypes::AxisDefaults axis_defaults(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::Move& m);
    ControllerTypes::Move move(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::AxisMove& m);
    ControllerTypes::AxisMove axis_move(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::AxisSpeed& s);
    ControllerTypes::AxisSpeed speed(p2p::Reader& r);
    void write(p2p::Writer& w, const ControllerTypes::Trajectory& t);
    ControllerTypes::Trajectory trajectory(p2p::Reader& r);
}
namespace p2p
{
    template<>
    struct Codec<control::protocol::Request>
    {
        static Bytes encode(const control::protocol::Request& v);
        static control::protocol::Request decode(std::span<const std::byte> b);
    };
    template<>
    struct Codec<control::protocol::Response>
    {
        static Bytes encode(const control::protocol::Response& v);
        static control::protocol::Response decode(std::span<const std::byte> b);
    };
    template<>
    struct Codec<control::protocol::Progress>
    {
        static Bytes encode(const control::protocol::Progress& v);
        static control::protocol::Progress decode(std::span<const std::byte> b);
    };
    template<>
    struct Codec<control::protocol::Outcome>
    {
        static Bytes encode(const control::protocol::Outcome& v);
        static control::protocol::Outcome decode(std::span<const std::byte> b);
    };
    template<>
    struct Codec<control::protocol::State>
    {
        static Bytes encode(const control::protocol::State& v);
        static control::protocol::State decode(std::span<const std::byte> b);
    };
    template<>
    struct Codec<control::protocol::FaultState>
    {
        static Bytes encode(const control::protocol::FaultState& v);
        static control::protocol::FaultState decode(std::span<const std::byte> b);
    };
}
namespace control::protocol
{
    template<typename T>
    concept Message = std::same_as<T, Request> || std::same_as<T, Response> || std::same_as<T, Progress> ||
                      std::same_as<T, Outcome> || std::same_as<T, State> || std::same_as<T, FaultState>;
}
namespace pnm::utils::memory
{
    // Local bus and wire use the same explicit codec, independent of native layout.
    template<control::protocol::Message T>
    struct SerializationAdapter<T>
    {
        static std::size_t bufferSize(const T& value) { return p2p::encode(value).size(); }
        static void serialize(const T& value, std::span<std::byte> output)
        {
            auto bytes = p2p::encode(value);
            if (output.size() != bytes.size())
                throw std::invalid_argument("control payload size");
            std::ranges::copy(bytes, output.begin());
        }
        static void deserialize(std::span<const std::byte> input, T& value) { value = p2p::decode<T>(input); }
    };
}
