#include "Protocol.hpp"
#include <cmath>
namespace control::protocol
{
    using T = ControllerTypes;
    using W = p2p::Writer;
    using R = p2p::Reader;
    namespace
    {
        template<typename E>
        void en(W& w, E value)
        {
            w.integer<std::uint8_t>(static_cast<std::uint8_t>(value));
        }
        template<typename E>
        E en(R& r, unsigned max)
        {
            auto n = r.integer<std::uint8_t>();
            if (n > max)
                throw std::invalid_argument("invalid control enum");
            return static_cast<E>(n);
        }
        // Stable semantic codes; Linux and newlib use different errno values.
        constexpr std::array WIRE_ERRORS{ std::errc::io_error,
                                          std::errc::no_such_device,
                                          std::errc::operation_not_supported,
                                          std::errc::invalid_argument,
                                          std::errc::result_out_of_range,
                                          std::errc::state_not_recoverable,
                                          std::errc::timed_out,
                                          std::errc::operation_canceled,
                                          std::errc::resource_unavailable_try_again,
                                          std::errc::no_message_available,
                                          std::errc::device_or_resource_busy,
                                          std::errc::message_size,
                                          std::errc::no_buffer_space,
                                          std::errc::not_connected,
                                          std::errc::not_supported,
                                          std::errc::operation_not_permitted,
                                          std::errc::permission_denied,
                                          std::errc::protocol_error,
                                          std::errc::value_too_large,
                                          std::errc::no_such_process };
        void error(W& w, std::error_code value)
        {
            unsigned code = value ? 1 : 0;
            for (unsigned i = 0; i < WIRE_ERRORS.size(); ++i)
                if (value == std::make_error_code(WIRE_ERRORS[i])) {
                    code = i + 1;
                    break;
                }
            w.integer<std::uint8_t>(static_cast<std::uint8_t>(code));
        }
        std::error_code error(R& r)
        {
            const auto code = r.integer<std::uint8_t>();
            if (!code)
                return {};
            if (code > WIRE_ERRORS.size())
                throw std::invalid_argument("invalid control error");
            return std::make_error_code(WIRE_ERRORS[code - 1]);
        }
        void driver_status(W& w, const hal::device::IStepperDriver::Status& s)
        {
            w.boolean(s.ready);
            w.boolean(s.fault_active);
            w.boolean(s.fault_latched);
            w.boolean(s.warning);
            en(w, s.fault);
            error(w, s.error);
        }
        hal::device::IStepperDriver::Status driver_status(R& r)
        {
            hal::device::IStepperDriver::Status s;
            s.ready = r.boolean();
            s.fault_active = r.boolean();
            s.fault_latched = r.boolean();
            s.warning = r.boolean();
            s.fault = en<hal::device::IStepperDriver::Fault>(
              r, static_cast<unsigned>(hal::device::IStepperDriver::Fault::Reset));
            s.error = error(r);
            return s;
        }
        void status(W& w, const T::Status& s)
        {
            w.boolean(s.enabled);
            en(w, s.generator.state);
            w.boolean(s.generator.counts_exact);
            w.boolean(s.coordinated);
            w.integer(s.configuration_revision);
            w.integer(s.command_revision);
            for (unsigned i = 0; i < 3; ++i) {
                w.integer(s.generator.pulses[i]);
                en(w, s.generator.axes[i]);
                w.integer<std::uint64_t>(static_cast<std::uint64_t>(s.generator.periods[i].count()));
            }
            for (const auto& a : s.axes) {
                en(w, a.motor);
                w.real(a.position.get());
                w.real(a.velocity.get());
                error(w, a.actual_position ? std::error_code{} : a.actual_position.error());
                w.real(a.actual_position ? a.actual_position->get() : 0);
                error(w, a.actual_velocity ? std::error_code{} : a.actual_velocity.error());
                w.real(a.actual_velocity ? a.actual_velocity->get() : 0);
                w.boolean(a.referenced);
                w.boolean(a.reference_switch_active);
                w.integer<std::uint32_t>(static_cast<std::uint32_t>(a.outstanding));
                w.boolean(a.conversion.has_value());
                if (a.conversion)
                    write(w, *a.conversion);
                en(w, a.feedback_source);
                w.real(a.feedback_resolution.get());
            }
        }
        T::Status status(R& r)
        {
            T::Status s{};
            s.enabled = r.boolean();
            s.generator.state = en<hal::step::State>(r, static_cast<unsigned>(hal::step::State::DmaError));
            s.generator.counts_exact = r.boolean();
            s.coordinated = r.boolean();
            s.configuration_revision = r.integer<std::uint64_t>();
            s.command_revision = r.integer<std::uint64_t>();
            for (unsigned i = 0; i < 3; ++i) {
                s.generator.pulses[i] = r.integer<std::uint64_t>();
                s.generator.axes[i] =
                  en<hal::step::State>(r, static_cast<unsigned>(hal::step::State::DmaError));
                s.generator.periods[i] = std::chrono::nanoseconds(r.integer<std::uint64_t>());
            }
            for (auto& a : s.axes) {
                a.motor = en<T::MotorId>(r, 2);
                a.position = pnm::units::Angle::create(r.real());
                a.velocity = pnm::units::AngularVelocity::create(r.real());
                auto e = error(r);
                auto v = r.real();
                a.actual_position =
                  e ? pnm::Result<pnm::units::Angle>(std::unexpected(e)) : pnm::units::Angle::create(v);
                e = error(r);
                v = r.real();
                a.actual_velocity = e ? pnm::Result<pnm::units::AngularVelocity>(std::unexpected(e))
                                      : pnm::units::AngularVelocity::create(v);
                a.referenced = r.boolean();
                a.reference_switch_active = r.boolean();
                a.outstanding = r.integer<std::uint32_t>();
                if (r.boolean())
                    a.conversion = conversion(r);
                a.feedback_source = en<hal::device::IMotorFeedback::Source>(r, 2);
                a.feedback_resolution = pnm::units::Angle::create(r.real());
            }
            return s;
        }
    }
    void routes(p2p::Bridge& b, bool io)
    {
        if (io) {
            b.exportService<Request, Response>(REQUEST_ENDPOINT, REQUEST_NAME);
            b.exportService<Request, Response>(STOP_ENDPOINT, STOP_NAME, p2p::Lane::Urgent);
            b.exportAction<Request, Progress, Outcome>(EXECUTE_ENDPOINT, EXECUTE_NAME);
            b.publishTopic<State>(STATE_ENDPOINT, STATE_NAME);
            b.publishTopic<FaultState>(FAULT_ENDPOINT, FAULT_NAME);
        }
        else {
            b.importService<Request, Response>(REQUEST_ENDPOINT, REQUEST_NAME);
            b.importService<Request, Response>(STOP_ENDPOINT, STOP_NAME, p2p::Lane::Urgent);
            b.importAction<Request, Progress, Outcome>(EXECUTE_ENDPOINT, EXECUTE_NAME);
            b.receiveTopic<State>(STATE_ENDPOINT, STATE_NAME);
            b.receiveTopic<FaultState>(FAULT_ENDPOINT, FAULT_NAME);
        }
    }
    void write(W& w, const T::Conversion& c)
    {
        w.boolean(std::holds_alternative<LinearAxisConversion>(c));
        std::visit([&](const auto& value) {
            const auto& cfg = value.configuration();
            if constexpr (std::same_as<std::decay_t<decltype(value)>, LinearAxisConversion>) {
                w.real(cfg.travel_per_output_revolution.get());
                w.real(cfg.motor_revolutions_per_output_revolution);
            }
            else
                w.real(cfg.motor_revolutions_per_axis_revolution);
            en(w, cfg.direction);
            w.real(cfg.motor_reference.get());
            w.real(cfg.axis_reference.get());
        }, c);
    }
    T::Conversion conversion(R& r)
    {
        if (r.boolean()) {
            LinearAxisConversion::Config c;
            c.travel_per_output_revolution = pnm::units::Distance::create(r.real());
            c.motor_revolutions_per_output_revolution = r.real();
            c.direction = en<AxisDirection>(r, 1);
            c.motor_reference = pnm::units::Angle::create(r.real());
            c.axis_reference = pnm::units::Distance::create(r.real());
            return LinearAxisConversion(c);
        }
        RotaryAxisConversion::Config c;
        c.motor_revolutions_per_axis_revolution = r.real();
        c.direction = en<AxisDirection>(r, 1);
        c.motor_reference = pnm::units::Angle::create(r.real());
        c.axis_reference = pnm::units::Angle::create(r.real());
        return RotaryAxisConversion(c);
    }
    void write(W& w, const MotionDefaults& d)
    {
        w.real(d.acceleration.get());
        w.real(d.deceleration.get());
        w.real(d.jerk.get());
    }
    MotionDefaults defaults(R& r)
    {
        MotionDefaults d;
        d.acceleration = pnm::units::AngularAcceleration::create(r.real());
        d.deceleration = pnm::units::AngularAcceleration::create(r.real());
        d.jerk = pnm::units::AngularJerk::create(r.real());
        return d;
    }
    void write(W& w, const T::AxisDefaults& d)
    {
        w.boolean(std::holds_alternative<T::LinearDefaults>(d));
        std::visit([&](const auto& v) {
            w.real(v.acceleration.get());
            w.real(v.deceleration.get());
            w.real(v.jerk.get());
        }, d);
    }
    T::AxisDefaults axis_defaults(R& r)
    {
        if (!r.boolean())
            return defaults(r);
        T::LinearDefaults d;
        d.acceleration = pnm::units::Acceleration::create(r.real());
        d.deceleration = pnm::units::Acceleration::create(r.real());
        d.jerk = pnm::units::Jerk::create(r.real());
        return d;
    }
    void write(W& w, const T::PlanningSnapshot& s)
    {
        status(w, s.status);
        for (const auto& d : s.defaults)
            write(w, d);
        for (const auto& c : s.configuration) {
            w.real(c.reference_position.get());
            w.real(c.full_step_angle.get());
            w.integer<std::uint32_t>(static_cast<std::uint32_t>(c.microsteps));
        }
    }
    T::PlanningSnapshot snapshot(R& r)
    {
        T::PlanningSnapshot s{};
        s.status = status(r);
        for (auto& d : s.defaults)
            d = defaults(r);
        for (auto& c : s.configuration) {
            c.reference_position = pnm::units::Angle::create(r.real());
            c.full_step_angle = pnm::units::Angle::create(r.real());
            c.microsteps = r.integer<std::uint32_t>();
        }
        return s;
    }
    void write(W& w, const T::DriverConfiguration& c)
    {
        w.integer(c.run_milliamps);
        w.integer(c.hold_milliamps);
        w.integer(c.microsteps);
        en(w, c.mode);
        w.boolean(c.interpolate);
        w.boolean(c.index_step);
    }
    T::DriverConfiguration driver_configuration(R& r)
    {
        T::DriverConfiguration c;
        c.run_milliamps = r.integer<std::uint16_t>();
        c.hold_milliamps = r.integer<std::uint16_t>();
        c.microsteps = r.integer<std::uint16_t>();
        c.mode = en<hal::device::IConfigurableStepperDriver::Mode>(r, 1);
        c.interpolate = r.boolean();
        c.index_step = r.boolean();
        return c;
    }
    void write(W& w, const std::array<T::DriverStatus, 3>& values)
    {
        for (const auto& v : values) {
            en(w, v.motor);
            w.text(v.name);
            driver_status(w, v.status);
            w.boolean(v.configuration.has_value());
            if (!v.configuration)
                continue;
            const auto& s = *v.configuration;
            write(w, s.configuration);
            w.boolean(s.diagnostics.has_value());
            if (s.diagnostics) {
                auto d = *s.diagnostics;
                w.integer(d.global);
                w.integer(d.driver);
                w.integer(d.input);
                w.integer(d.load);
            }
            w.integer(s.limits.maximum_run_milliamps);
            w.boolean(s.limits.hold_equals_run);
            w.integer(s.address);
            w.integer(s.nominal_run_milliamps);
            w.integer(s.nominal_hold_milliamps);
        }
    }
    std::array<T::DriverStatus, 3> drivers(R& r)
    {
        std::array<T::DriverStatus, 3> values{};
        for (auto& v : values) {
            v.motor = en<T::MotorId>(r, 2);
            v.name = r.text();
            v.status = driver_status(r);
            if (!r.boolean())
                continue;
            v.configuration.emplace();
            auto& s = *v.configuration;
            s.configuration = driver_configuration(r);
            if (r.boolean()) {
                s.diagnostics.emplace();
                auto& d = *s.diagnostics;
                d.global = r.integer<std::uint32_t>();
                d.driver = r.integer<std::uint32_t>();
                d.input = r.integer<std::uint32_t>();
                d.load = r.integer<std::uint16_t>();
            }
            s.limits.maximum_run_milliamps = r.integer<std::uint16_t>();
            s.limits.hold_equals_run = r.boolean();
            s.address = r.integer<std::uint8_t>();
            s.nominal_run_milliamps = r.integer<std::uint16_t>();
            s.nominal_hold_milliamps = r.integer<std::uint16_t>();
        }
        return values;
    }
    void write(W& w, const T::Move& m)
    {
        w.real(m.position.get());
        w.real(m.velocity.get());
        w.real(m.acceleration.get());
        w.real(m.deceleration.get());
        w.real(m.jerk.get());
        en(w, m.buffer);
        w.real(m.timeout.get());
        w.boolean(m.absolute);
    }
    T::Move move(R& r)
    {
        T::Move m{};
        m.position = pnm::units::Angle::create(r.real());
        m.velocity = pnm::units::AngularVelocity::create(r.real());
        m.acceleration = pnm::units::AngularAcceleration::create(r.real());
        m.deceleration = pnm::units::AngularAcceleration::create(r.real());
        m.jerk = pnm::units::AngularJerk::create(r.real());
        m.buffer = en<BufferMode>(r, static_cast<unsigned>(BufferMode::BlendingHigh));
        m.timeout = pnm::units::Time::create(r.real());
        m.absolute = r.boolean();
        return m;
    }
    void write(W& w, const T::AxisMove& m)
    {
        w.boolean(std::holds_alternative<T::LinearMove>(m));
        std::visit([&](const auto& v) {
            T::Move x{ pnm::units::Angle::create(v.position.get()),
                       pnm::units::AngularVelocity::create(v.velocity.get()),
                       pnm::units::AngularAcceleration::create(v.acceleration.get()),
                       pnm::units::AngularAcceleration::create(v.deceleration.get()),
                       pnm::units::AngularJerk::create(v.jerk.get()),
                       v.buffer,
                       v.timeout,
                       v.absolute };
            write(w, x);
        }, m);
    }
    T::AxisMove axis_move(R& r)
    {
        auto linear = r.boolean();
        auto m = move(r);
        if (!linear)
            return m;
        return T::LinearMove{ pnm::units::Distance::create(m.position.get()),
                              pnm::units::Velocity::create(m.velocity.get()),
                              pnm::units::Acceleration::create(m.acceleration.get()),
                              pnm::units::Acceleration::create(m.deceleration.get()),
                              pnm::units::Jerk::create(m.jerk.get()),
                              m.buffer,
                              m.timeout,
                              m.absolute };
    }
    void write(W& w, const T::AxisSpeed& s)
    {
        w.boolean(std::holds_alternative<pnm::units::Velocity>(s));
        std::visit([&](auto v) { w.real(v.get()); }, s);
    }
    T::AxisSpeed speed(R& r)
    {
        auto linear = r.boolean();
        auto v = r.real();
        if (linear)
            return pnm::units::Velocity::create(v);
        return pnm::units::AngularVelocity::create(v);
    }
    void write(W& w, const T::Trajectory& t)
    {
        w.integer(t.configuration_revision);
        w.integer(t.command_revision);
        for (auto v : t.origin)
            w.real(v.get());
        if (t.profile.size > motion::Profile::MAX_PHASES)
            throw std::invalid_argument("too many trajectory phases");
        w.integer<std::uint8_t>(static_cast<std::uint8_t>(t.profile.size));
        for (unsigned i = 0; i < t.profile.size; ++i) {
            const auto& p = t.profile.phases[i];
            w.real(p.duration);
            w.real(p.jerk);
            w.real(p.position);
            w.real(p.velocity);
            w.real(p.acceleration);
        }
        w.real(t.profile.distance);
        w.real(t.profile.end_velocity);
        for (auto n : t.pulses)
            w.integer(n);
        for (bool f : t.forward)
            w.boolean(f);
        w.real(t.duration.get());
    }
    T::Trajectory trajectory(R& r)
    {
        T::Trajectory t{};
        t.configuration_revision = r.integer<std::uint64_t>();
        t.command_revision = r.integer<std::uint64_t>();
        for (auto& v : t.origin)
            v = pnm::units::Angle::create(r.real());
        t.profile.size = r.integer<std::uint8_t>();
        if (t.profile.size > motion::Profile::MAX_PHASES)
            throw std::invalid_argument("too many trajectory phases");
        for (unsigned i = 0; i < t.profile.size; ++i) {
            auto& p = t.profile.phases[i];
            p.duration = r.real();
            p.jerk = r.real();
            p.position = r.real();
            p.velocity = r.real();
            p.acceleration = r.real();
        }
        t.profile.distance = r.real();
        t.profile.end_velocity = r.real();
        for (auto& n : t.pulses)
            n = r.integer<std::uint64_t>();
        for (auto& f : t.forward)
            f = r.boolean();
        t.duration = pnm::units::Time::create(r.real());
        return t;
    }
}
namespace p2p
{
    using namespace control::protocol;
    Bytes Codec<Request>::encode(const Request& v)
    {
        Writer w;
        w.integer<std::uint8_t>(static_cast<std::uint8_t>(v.operation));
        w.integer(v.motor);
        w.bytes.insert(w.bytes.end(), v.data.begin(), v.data.end());
        return std::move(w.bytes);
    }
    Request Codec<Request>::decode(std::span<const std::byte> b)
    {
        Reader r(b);
        Request v;
        auto op = r.integer<std::uint8_t>();
        if (op > static_cast<unsigned>(Operation::Trajectory))
            throw std::invalid_argument("invalid control operation");
        v.operation = static_cast<Operation>(op);
        v.motor = r.integer<std::uint8_t>();
        if (v.motor > 2 && v.motor != control::protocol::ALL_MOTORS)
            throw std::invalid_argument("invalid motor ID");
        v.data.assign(r.remaining().begin(), r.remaining().end());
        return v;
    }
    Bytes Codec<Response>::encode(const Response& v)
    {
        Writer w;
        w.boolean(v.success);
        w.text(v.error);
        w.bytes.insert(w.bytes.end(), v.data.begin(), v.data.end());
        return std::move(w.bytes);
    }
    Response Codec<Response>::decode(std::span<const std::byte> b)
    {
        Reader r(b);
        Response v;
        v.success = r.boolean();
        v.error = r.text();
        v.data.assign(r.remaining().begin(), r.remaining().end());
        return v;
    }
    Bytes Codec<Progress>::encode(const Progress& v)
    {
        Writer w;
        w.integer(v.execution_id);
        w.integer(v.pulses);
        w.real(v.position);
        w.boolean(v.referenced);
        return std::move(w.bytes);
    }
    Progress Codec<Progress>::decode(std::span<const std::byte> b)
    {
        Reader r(b);
        Progress v;
        v.execution_id = r.integer<std::uint64_t>();
        v.pulses = r.integer<std::uint64_t>();
        v.position = r.real();
        v.referenced = r.boolean();
        r.finish();
        return v;
    }
    Bytes Codec<Outcome>::encode(const Outcome& v)
    {
        Writer w;
        w.integer<std::uint8_t>(static_cast<std::uint8_t>(v.result));
        w.text(v.error);
        w.boolean(v.invalid_argument);
        return std::move(w.bytes);
    }
    Outcome Codec<Outcome>::decode(std::span<const std::byte> b)
    {
        Reader r(b);
        Outcome v;
        auto result = r.integer<std::uint8_t>();
        if (result > 4)
            throw std::invalid_argument("invalid motion outcome");
        v.result = static_cast<control::MotorResult>(result);
        v.error = r.text();
        v.invalid_argument = r.boolean();
        r.finish();
        return v;
    }
    Bytes Codec<State>::encode(const State& v)
    {
        Writer w;
        write(w, v.snapshot);
        return std::move(w.bytes);
    }
    State Codec<State>::decode(std::span<const std::byte> b)
    {
        Reader r(b);
        State v{ snapshot(r) };
        r.finish();
        return v;
    }
    Bytes Codec<FaultState>::encode(const FaultState& v)
    {
        Writer w;
        for (const auto& s : v.drivers)
            control::protocol::driver_status(w, s);
        w.boolean(v.link_inhibited);
        return std::move(w.bytes);
    }
    FaultState Codec<FaultState>::decode(std::span<const std::byte> b)
    {
        Reader r(b);
        FaultState v;
        for (auto& s : v.drivers)
            s = control::protocol::driver_status(r);
        v.link_inhibited = r.boolean();
        r.finish();
        return v;
    }
}
