#pragma once

#include "ControllerTypes.hpp"
#include "p2p/transport/ITransport.hpp"
#include <pneumo/messaging.hpp>

namespace control::messages
{
    // These value-only messages use pneumo's automatic serialization. Both peers
    // must use matching type definitions and ABI; this is not a portable wire format.
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

    // error_code owns a process-local category pointer. Messages carry its error
    // condition by value, and reconstruct the local generic category on receipt.
    template<typename T>
    struct Measurement
    {
        T value{};
        std::errc error{};
        Measurement() = default;
        explicit Measurement(const pnm::Result<T>& result)
          : value(result.value_or(T{}))
          , error(result ? std::errc{}
                         : static_cast<std::errc>(result.error().default_error_condition().value()))
        {
        }
        pnm::Result<T> result() const
        {
            if (error != std::errc{})
                return std::unexpected(std::make_error_code(error));
            return value;
        }
    };
    struct DriverState
    {
        bool ready{}, fault_active{}, fault_latched{}, warning{};
        hal::device::IStepperDriver::Fault fault{};
        std::errc error{};
        DriverState() = default;
        explicit DriverState(const hal::device::IStepperDriver::Status& s)
          : ready(s.ready)
          , fault_active(s.fault_active)
          , fault_latched(s.fault_latched)
          , warning(s.warning)
          , fault(s.fault)
          , error(static_cast<std::errc>(s.error.default_error_condition().value()))
        {
        }
        hal::device::IStepperDriver::Status status() const
        {
            return { ready, fault_active, fault_latched, warning, fault, std::make_error_code(error) };
        }
    };
    struct AxisState
    {
        ControllerTypes::MotorId motor{};
        pnm::units::Angle position{};
        pnm::units::AngularVelocity velocity{};
        Measurement<pnm::units::Angle> actual_position{};
        Measurement<pnm::units::AngularVelocity> actual_velocity{};
        bool referenced{}, reference_switch_active{};
        std::size_t outstanding{};
        std::optional<ControllerTypes::Conversion> conversion{};
        hal::device::IMotorFeedback::Source feedback_source{};
        pnm::units::Angle feedback_resolution{};
        AxisState() = default;
        explicit AxisState(const ControllerTypes::AxisStatus& s)
          : motor(s.motor)
          , position(s.position)
          , velocity(s.velocity)
          , actual_position(s.actual_position)
          , actual_velocity(s.actual_velocity)
          , referenced(s.referenced)
          , reference_switch_active(s.reference_switch_active)
          , outstanding(s.outstanding)
          , conversion(s.conversion)
          , feedback_source(s.feedback_source)
          , feedback_resolution(s.feedback_resolution)
        {
        }
        ControllerTypes::AxisStatus status() const
        {
            return { motor,
                     position,
                     velocity,
                     actual_position.result(),
                     actual_velocity.result(),
                     referenced,
                     reference_switch_active,
                     outstanding,
                     conversion,
                     feedback_source,
                     feedback_resolution };
        }
    };
    struct State
    {
        bool enabled{};
        hal::step::Status generator{};
        std::array<AxisState, 3> axes{};
        bool coordinated{};
        std::uint64_t configuration_revision{}, command_revision{};
        std::array<MotionDefaults, 3> defaults{};
        std::array<ControllerTypes::AxisConfig, 3> configuration{};
        State() = default;
        explicit State(const ControllerTypes::PlanningSnapshot& s)
          : enabled(s.status.enabled)
          , generator(s.status.generator)
          , coordinated(s.status.coordinated)
          , configuration_revision(s.status.configuration_revision)
          , command_revision(s.status.command_revision)
          , defaults(s.defaults)
          , configuration(s.configuration)
        {
            for (std::size_t i = 0; i < axes.size(); ++i)
                axes[i] = AxisState(s.status.axes[i]);
        }
        ControllerTypes::PlanningSnapshot snapshot() const
        {
            ControllerTypes::PlanningSnapshot result{};
            result.status.enabled = enabled;
            result.status.generator = generator;
            for (std::size_t i = 0; i < axes.size(); ++i)
                result.status.axes[i] = axes[i].status();
            result.status.coordinated = coordinated;
            result.status.configuration_revision = configuration_revision;
            result.status.command_revision = command_revision;
            result.defaults = defaults;
            result.configuration = configuration;
            return result;
        }
    };
    struct DriverStatus
    {
        ControllerTypes::MotorId motor{};
        std::string name{};
        DriverState state{};
        std::optional<hal::device::IConfigurableStepperDriver::Snapshot> configuration{};
        DriverStatus() = default;
        explicit DriverStatus(const ControllerTypes::DriverStatus& s)
          : motor(s.motor)
          , name(s.name)
          , state(s.status)
          , configuration(s.configuration)
        {
        }
        ControllerTypes::DriverStatus status() const
        {
            return { motor, name, state.status(), configuration };
        }
    };

    // Services handle short control/configuration requests. The operation selects
    // which typed field is relevant; unused fields retain their defaults.
    struct Request
    {
        Operation operation{};
        std::uint8_t motor{ ALL_MOTORS };
        ControllerTypes::DriverConfiguration driver{};
        std::optional<ControllerTypes::Conversion> conversion{};
        MotionDefaults defaults{};
        ControllerTypes::AxisDefaults axis_defaults{};
        pnm::units::AngularVelocity velocity{};
        ControllerTypes::AxisSpeed axis_velocity{};
    };
    struct Response
    {
        bool success{ true };
        std::string error;
        State state{};
        std::array<DriverStatus, 3> drivers{};
        ControllerTypes::AxisDefaults axis_defaults{};
        std::uint64_t pulses{};
    };
    // Motion is a separate action contract; no byte buffers or worker handles.
    struct Goal
    {
        Operation operation{ Operation::Move };
        std::uint8_t motor{ ALL_MOTORS };
        ControllerTypes::Move move{};
        ControllerTypes::AxisMove axis_move{};
        pnm::units::AngularVelocity seek{ DEFAULT_REFERENCE_SEEK }, latch{ DEFAULT_REFERENCE_LATCH };
        std::optional<ControllerTypes::AxisSpeed> axis_seek{}, axis_latch{};
        ControllerTypes::Trajectory trajectory{};
        pnm::units::Time timeout{ DEFAULT_REFERENCE_TIMEOUT };
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
    struct FaultState
    {
        std::array<DriverState, 3> drivers{};
        bool link_inhibited{};
    };

    enum class Controller
    {
        IO,
        UI
    };

    using TopicPublisher = Controller;
    using ServiceProvider = Controller;
    using ActionProvider = Controller;

    using namespace pnm::meta::string::literals;
    struct Topics
    {
        [[ = TopicPublisher::IO, = "motion.state"_fs ]] pnm::msg::Topic<State> state;
        [[ = TopicPublisher::IO, = "motion.fault"_fs ]] pnm::msg::Topic<FaultState> fault;
    };
    struct Services
    {
        [[ = ServiceProvider::IO, = "motion.request"_fs ]] pnm::msg::Service<Request, Response> request;
        [[ = ServiceProvider::IO,
           = "motion.stop"_fs,
           = p2p::Lane::Urgent ]] pnm::msg::Service<Request, Response>
          stop;
    };
    struct Actions
    {
        [[ = ActionProvider::IO, = "motion.execute"_fs ]] pnm::msg::Action<Goal, Progress, Outcome> execute;
    };
}
