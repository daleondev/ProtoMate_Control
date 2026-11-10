#include "ControllerLink.hpp"
#include "Catalog.hpp"
#include "SharedTransport.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "runtime/thread.hpp"
#include "simulation/Client.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/motion/RobotConfiguration.hpp"
#include <cmath>
#include <map>
#include <ostream>
namespace simulation
{
    namespace
    {
        constexpr auto REQUEST_TIMEOUT{ std::chrono::seconds{ 3 } },
          PEER_TIMEOUT{ std::chrono::seconds{ 10 } };
        constexpr auto HEARTBEAT{ std::chrono::milliseconds{ 250 } },
          SHUTDOWN_TIMEOUT{ std::chrono::milliseconds{ 100 } };
        constexpr std::size_t WORKER_STACK{ 65536 }, SLIP_ARGUMENTS{ 4 };
        constexpr double MAX_SLIP_DEGREES{ 36000 }, NANOSECONDS_PER_SECOND{ 1e9 };
        using namespace pnm::units;
        using namespace pnm::units::literals;
        constexpr auto ROTARY_HOME_SPEED{ 60_rpm }, LINEAR_HOME_SPEED{ 180_rpm }, HOME_LATCH_SPEED{ 5_rpm };
        constexpr auto HOME_TIMEOUT{ 90_s };
        constexpr ui::motion::RotaryDynamics ROTARY_DYNAMICS{ .velocity = 20_deg / 1_s,
                                                              .acceleration = 100_deg_s2,
                                                              .deceleration = 100_deg_s2,
                                                              .jerk = 1000_deg_s3 };
        constexpr ui::motion::LinearDynamics LINEAR_DYNAMICS{ .velocity = 4_mm_s,
                                                              .acceleration = 20_mm_s2,
                                                              .deceleration = 20_mm_s2,
                                                              .jerk = 200_mm_s3 };
        constexpr auto WORKSPACE_EXTENT{ 600_mm }, PATH_TOLERANCE{ .1_mm };
        constexpr ui::motion::JointPosition NEUTRAL_POSITION{ 0_deg, -60_deg, 15_mm };
        // Process-local bridge binding; installed and cleared before/after its worker lifetime.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        pnm::msg::Bus* active_bus{};
        std::optional<ui::motion::RobotConfiguration> configuration_for(
          const motion::trajectory::Capabilities& hardware)
        {
            if (!active_bus) {
                return {};
            }
            auto call{ active_bus->service<^^Catalog::control>().request(
              Packet{ .operation = Operation::Configuration }, REQUEST_TIMEOUT) };
            auto response{ call.get() };
            if (!response || response->status || response->size != sizeof(MechanicalConfiguration)) {
                return {};
            }
            MechanicalConfiguration v{};
            for (unsigned i{ 0 }; i < v.size(); ++i) {
                v[i] = std::bit_cast<double>(get(response->data,
                                                 static_cast<std::size_t>(i) * state_wire::VALUE_BYTES,
                                                 state_wire::VALUE_BYTES));
            }
            using namespace pnm::units;
            using namespace pnm::units::literals;
            auto c{ ui::motion::simulation_configuration(hardware, true) };
            std::array<double, 3> home{};
            for (unsigned i{ 0 }; i < 3; ++i) {
                const bool motor_positive{ (hardware.reference_forward_mask & (1U << i)) != 0 };
                const bool joint_positive{ motor_positive != (unsigned(v[mechanical::DIRECTION + i]) != 0U) };
                home[i] = v[(joint_positive ? mechanical::UPPER_CONTACT : mechanical::LOWER_CONTACT) + i];
                c.homing[i] = { .configured = true,
                                .positive = motor_positive,
                                .seek = i == 2 ? LINEAR_HOME_SPEED : ROTARY_HOME_SPEED,
                                .latch = HOME_LATCH_SPEED,
                                .timeout = HOME_TIMEOUT };
            }
            for (unsigned i{ 0 }; i < 2; ++i) {
                c.rotary[i].motor_revolutions_per_axis_revolution = v[i];
                c.rotary[i].direction =
                  static_cast<ui::motion::AxisDirection>(unsigned(v[mechanical::DIRECTION + i]));
                c.rotary[i].axis_reference = Angle::create(home[i]);
                c.rotary_dynamics[i] = ROTARY_DYNAMICS;
            }
            c.linear.travel_per_output_revolution = v[mechanical::LEAD] * 1_mm;
            c.linear.motor_revolutions_per_output_revolution = v[2];
            c.linear.direction =
              static_cast<ui::motion::AxisDirection>(unsigned(v[mechanical::DIRECTION + 2]));
            c.linear.axis_reference = Distance::create(home[2]);
            c.linear_dynamics = LINEAR_DYNAMICS;
            c.geometry.first_arm_length = v[mechanical::GEOMETRY] * 1_mm;
            c.geometry.second_arm_length = v[mechanical::GEOMETRY + 1] * 1_mm;
            c.geometry.tool_offset = { .x = v[mechanical::GEOMETRY + 2] * 1_mm,
                                       .y = 0_mm,
                                       .z = v[mechanical::TOOL_Z] * 1_mm };
            c.geometry.limits = ui::motion::ScaraKinematics::JointLimits{
                .minimum = { .shoulder = Angle::create(v[mechanical::SOFT_MINIMUM]),
                             .elbow = Angle::create(v[mechanical::SOFT_MINIMUM + 1]),
                             .z = Distance::create(v[mechanical::SOFT_MINIMUM + 2]) },
                .maximum = { .shoulder = Angle::create(v[mechanical::SOFT_MAXIMUM]),
                             .elbow = Angle::create(v[mechanical::SOFT_MAXIMUM + 1]),
                             .z = Distance::create(v[mechanical::SOFT_MAXIMUM + 2]) }
            };
            c.workspace_minimum = { .x = -WORKSPACE_EXTENT,
                                    .y = -WORKSPACE_EXTENT,
                                    .z = v[mechanical::TOOL_Z] * 1_mm };
            c.workspace_maximum = { .x = WORKSPACE_EXTENT,
                                    .y = WORKSPACE_EXTENT,
                                    .z = v[mechanical::TOOL_Z] * 1_mm +
                                         Distance::create(v[mechanical::SOFT_MAXIMUM + 2]) };
            c.path_tolerance = PATH_TOLERANCE;
            c.neutral_position = NEUTRAL_POSITION;
            return c;
        }
        ui::cli::CallbackResult parse_fault(const ui::cli::Arguments& args, Packet& q)
        {
            static const std::map<std::string_view, Fault> names{
                { "clear", Fault::None },
                { "alarm", Fault::DriverAlarm },
                { "reference-stuck", Fault::ReferenceStuck },
                { "reference-open", Fault::ReferenceDisconnected },
                { "encoder-open", Fault::EncoderDisconnected },
                { "lost-step", Fault::LostStep },
                { "slip", Fault::ShaftSlip },
                { "dma-delay", Fault::DmaDelay },
                { "irq-delay", Fault::InterruptDelay },
                { "uart-open", Fault::UartDisconnected },
                { "uart-corrupt", Fault::UartCorrupt }
            };
            if (args.size() < 3 || args.at(1).size() != 2 || args.at(1)[0] != 'm' || args.at(1)[1] < '1' ||
                args.at(1)[1] > '3' || !names.contains(args.at(2))) {
                return ui::cli::callback_failure(
                  "usage: sim fault m1|m2|m3 "
                  "clear|alarm|reference-stuck|reference-open|encoder-open|lost-step|slip|dma-delay|irq-"
                  "delay|uart-open|uart-corrupt [degrees]");
            }
            q.operation = Operation::Fault;
            q.arguments[0] = args.at(1)[1] - '1';
            q.arguments[1] = std::to_underlying(names.at(args.at(2)));
            if (args.at(2) == "slip") {
                if (args.size() != SLIP_ARGUMENTS) {
                    return ui::cli::callback_failure("slip requires signed shaft degrees");
                }
                std::size_t consumed{};
                const auto value{ std::stod(std::string{ args.at(3) }, &consumed) };
                if (consumed != args.at(3).size() || !std::isfinite(value) ||
                    std::abs(value) > MAX_SLIP_DEGREES) {
                    return ui::cli::callback_failure("invalid slip");
                }
                q.arguments[2] = std::bit_cast<std::uint64_t>(value);
            }
            return {};
        }
        ui::cli::CallbackResult control_command(const ui::cli::Arguments& args, std::ostream& out)
        {
            if (!active_bus) {
                return ui::cli::callback_failure("simulator is unavailable");
            }
            Packet q{ .operation = Operation::State };
            if (args.at(0) == "power") {
                if (args.size() != 2 || (args.at(1) != "on" && args.at(1) != "off")) {
                    return ui::cli::callback_failure("usage: sim power on|off");
                }
                q.operation = Operation::Power;
                q.arguments[0] = args.at(1) == "on";
            }
            else if (args.at(0) == "fault") {
                if (auto parsed{ parse_fault(args, q) }; !parsed) {
                    return parsed;
                }
            }
            else if (args.at(0) != "status") {
                return ui::cli::callback_failure("usage: sim status | sim power on|off | sim fault ...");
            }
            auto pending{ active_bus->service<^^Catalog::control>().request(q, REQUEST_TIMEOUT) };
            auto reply{ pending.get() };
            if (!reply || reply->status) {
                return ui::cli::callback_failure(
                  "simulator request failed; outcome must be reconciled with sim status");
            }
            if (q.operation == Operation::State) {
                out << "Native simulation (NOT calibrated) time="
                    << static_cast<double>(reply->time_ns) / NANOSECONDS_PER_SECOND
                    << " s enable=" << reply->arguments[2] << " power=" << reply->arguments[3]
                    << " references=" << reply->arguments[0] << " faults=" << reply->arguments[1] << '\n';
                for (unsigned i{}; i < 3; ++i) {
                    out << "m" << i + 1 << " shaft="
                        << std::bit_cast<double>(get(reply->data,
                                                     static_cast<std::size_t>(i) * state_wire::VALUE_BYTES,
                                                     state_wire::VALUE_BYTES))
                        << " deg; joint="
                        << std::bit_cast<double>(
                             get(reply->data,
                                 state_wire::JOINT + (std::size_t(i) * state_wire::VALUE_BYTES),
                                 state_wire::VALUE_BYTES))
                        << (i == 2 ? " mm" : " rad") << "; TCP " << "XYZ"[i] << "="
                        << std::bit_cast<double>(
                             get(reply->data,
                                 state_wire::TCP + (std::size_t(i) * state_wire::VALUE_BYTES),
                                 state_wire::VALUE_BYTES))
                        << " mm; rejected pulses="
                        << get(reply->data,
                               state_wire::REJECTED + (std::size_t(i) * state_wire::VALUE_BYTES),
                               state_wire::VALUE_BYTES)
                        << "\n";
                }
            }
            else {
                out << "Simulator change acknowledged at "
                    << static_cast<double>(reply->time_ns) / NANOSECONDS_PER_SECOND << " s.\n";
            }
            return {};
        }
    }
    struct ControllerLink::Impl
    {
        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;
        Impl(Impl&&) = delete;
        Impl& operator=(Impl&&) = delete;
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        SharedTransport transport{ *client::memory(), 1, context };
        std::jthread runner;
        icc::Bridge bridge{ bus, context, transport, { .heartbeat = HEARTBEAT, .timeout = PEER_TIMEOUT } };
        Impl()
        {
            runner = runtime::thread::create_jthread({ .name = "sim-icc", .stack_size = WORKER_STACK },
                                                     [this](const std::stop_token& token) {
                std::stop_callback stop{ token, [this] { context.stop(); } };
                context.run();
            });
            bridge.registerServices<Catalog>(Provider::Controller);
            bridge.start();
            active_bus = &bus;
            ui::motion::set_simulation_configuration_provider(configuration_for);
        }
        ~Impl()
        {
            ui::motion::set_simulation_configuration_provider({});
            active_bus = nullptr;
            bridge.stop(SHUTDOWN_TIMEOUT);
        }
    };
    ControllerLink::ControllerLink()
      : m_impl{ std::make_unique<Impl>() }
    {
    }
    ControllerLink::~ControllerLink() = default;
    void setup_commands(ui::cli::Parser& parser)
    {
        const auto registration{ parser.registerCommand(
          { .name = "sim",
            .description = "Native hardware simulator controls; mechanical state is provisional",
            .arguments = { { .name = "operation", .description = "status, power, fault" },
                           { .name = "values",
                             .description = "power on|off; fault m1|m2|m3 name [slip-degrees]",
                             .optional = true,
                             .variadic = true } },
            .flags = {},
            .callback = control_command }) };
        if (!registration) {
            throw std::runtime_error{ registration.error() };
        }
    }
}
