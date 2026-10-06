#include "MotionController.hpp"
#include "runtime/thread.hpp"
#include "pneumo/pneumo.hpp"

#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#endif

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace control
{
    using namespace std::chrono_literals;

    MotionController::MotionController(const std::array<AxisConfig, 3>& configuration)
      : m_configuration{ configuration }
      , m_enable{ hal::board::createSteppersEnableOutput() }
      , m_generator{ hal::board::createStepperGenerator() }
    {
        if (!m_enable || !m_generator)
            throw std::runtime_error("step generator/enable creation failed");
        m_enable->write(hal::gpio::Level::High);
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            const auto& config{ configuration[i] };
            m_motors[i] = std::make_unique<StepperMotor>(static_cast<MotorId>(i),
                                                         config.reference_position,
                                                         config.full_step_angle,
                                                         config.microsteps,
                                                         m_generator);
            m_motors[i]->m_events->groupNotification.store(&m_groupNotification);
        }
        if (const auto result{ m_generator->start() }; !result)
            throw std::runtime_error("step timebase start failed: " + result.error().message());
        m_driverBus = hal::board::createStepperDriverBus();
        if (!m_driverBus) throw std::runtime_error("stepper UART creation failed");
        m_alarm = hal::board::createStepperDiagnostic(MotorId::Motor1);
        if (!m_alarm) throw std::runtime_error("DM542T ALM creation failed");
        for (std::size_t i{}; i < 2; ++i) {
            const auto microsteps = configuration[i + 1].microsteps;
            if (microsteps == 0 || microsteps > 256 || (microsteps & (microsteps - 1)))
                throw std::invalid_argument("TMC2209 microsteps must be a power of two in 1..256");
            m_drivers[i] = std::make_unique<hal::device::Tmc2209>(m_driverBus, i);
            m_driverConfigurations[i].microsteps = static_cast<std::uint16_t>(microsteps);
            m_diagnostics[i] = hal::board::createStepperDiagnostic(static_cast<MotorId>(i + 1));
            if (!m_diagnostics[i]) throw std::runtime_error("stepper DIAG creation failed");
        }
        // Missing/unpowered drivers leave the console usable for diagnosis.
        try { initializeDriversLocked(); }
        catch (const std::exception& e) { pnm::log::error("Stepper driver initialization: {}", e.what()); }
        m_driverWorker = runtime::thread::create_jthread(
            // Below motion workers (16): polling must not delay switch stops.
            // ALM/DIAG disable the shared output immediately in interrupt context.
            { .name = "Driver monitor", .priority = 18, .stack_size = 8192 },
            [this](std::stop_token stop) { monitorDrivers(stop); });
        m_alarm->setEdgeCallback([this](hal::gpio::Level level) noexcept {
            if (level != hal::gpio::Level::High) return;
            m_driverFaults.fetch_or(AlarmFault, std::memory_order_release);
            m_enable->write(hal::gpio::Level::High);
            m_driverNotification.signal();
        });
        for (std::size_t i{}; i < 2; ++i)
            m_diagnostics[i]->setEdgeCallback([this, i](hal::gpio::Level level) noexcept {
                if (level != hal::gpio::Level::High) return;
                m_driverFaults.fetch_or(1U << i, std::memory_order_release);
                // ISR performs no UART, allocation, logging or mutex operations.
                m_enable->write(hal::gpio::Level::High);
                m_driverNotification.signal();
            });
    }

    MotionController::~MotionController()
    {
        m_alarm->setEdgeCallback({});
        for (auto& input : m_diagnostics) input->setEdgeCallback({});
        m_driverWorker.request_stop();
        m_driverNotification.signal();
        if (m_driverWorker.joinable()) m_driverWorker.join();
        disable();
        static_cast<void>(m_generator->stop());
    }

    StepperMotor& MotionController::axis(MotorId motor) const
    {
        const auto index{ static_cast<std::size_t>(motor) };
        if (index >= m_motors.size())
            throw std::invalid_argument("invalid motor");
        return *m_motors[index];
    }

    void MotionController::requireEnabled() const
    {
        if (!m_driversReady || m_driverFaults.load(std::memory_order_acquire) ||
            m_alarm->read() == hal::gpio::Level::High)
            throw std::runtime_error("stepper driver fault/not initialized; inspect 'motor driver status' then reset while disabled");
        if (m_enable->read() != hal::gpio::Level::Low)
            throw std::runtime_error("drivers disabled; enable drivers before requesting motion");
        if (m_generator->status().state != hal::step::State::Running)
            throw std::runtime_error("step generator fault/stopped; disable, reset and re-reference");
    }

    void MotionController::enable()
    {
        std::scoped_lock lock{ m_mutex };
        if (m_generator->status().state != hal::step::State::Running)
            throw std::runtime_error("step generator fault/stopped; disable drivers and reset the timebase");
        if ((m_driverFaults.load() & AlarmFault) || m_alarm->read() == hal::gpio::Level::High) {
            alarmFaultLocked();
            throw std::runtime_error("M1 DM542T ALM fault/open cable; clear cause, then reset while disabled");
        }
        if (!m_driversReady || m_driverFaults.load())
            throw std::runtime_error("drivers not ready; inspect 'motor driver status' and run 'motor driver init'");
        if (m_enable->read() == hal::gpio::Level::Low)
            return;
        // Catch a reset/disconnection since the last periodic poll before EN.
        for (std::size_t i{}; i < 2; ++i) {
            auto verified = m_drivers[i]->verify();
            auto state = m_drivers[i]->status();
            if (!verified || !state || state->fault() || state->reset() ||
                m_diagnostics[i]->read() == hal::gpio::Level::High) {
                driverFaultLocked(i, "driver verification failed before enable");
                throw std::runtime_error(m_driverErrors[i]);
            }
        }
        bool permitted{};
        {
#ifdef HAL_PLATFORM_STM32
            // A fault interrupt must never be overwritten by our enable write.
            const hal::stm32::InterruptGuard guard;
#endif
            unsigned observed = m_alarm->read() == hal::gpio::Level::High ? AlarmFault : 0U;
            for (std::size_t i{}; i < m_diagnostics.size(); ++i)
                if (m_diagnostics[i]->read() == hal::gpio::Level::High)
                    observed |= 1U << i;
            permitted = (m_driverFaults.fetch_or(observed) | observed) == 0;
            if (permitted)
                m_enable->write(hal::gpio::Level::Low);
        }
        if (!permitted) {
            m_driverNotification.signal();
            throw std::runtime_error("driver fault before enable");
        }
        std::this_thread::sleep_for(200ms);
        if ((m_driverFaults.load() & AlarmFault) || m_alarm->read() == hal::gpio::Level::High) {
            alarmFaultLocked();
            throw std::runtime_error("M1 DM542T ALM fault during enable settling");
        }
        if (m_driverFaults.load() || m_enable->read() != hal::gpio::Level::Low) {
            driverFaultLocked((m_driverFaults.load() & 2U) ? 1 : 0, "driver fault during enable settling");
            throw std::runtime_error("driver fault during enable settling");
        }
    }

    void MotionController::stopLocked(std::optional<MotorId> motor)
    {
        if (motor)
            static_cast<void>(axis(*motor));
        if (m_groupActive.load()) {
            // Serialize cancellation with the worker's arm/reference submission.
            // Release this lock before joining: the worker never takes m_mutex.
            {
                std::scoped_lock action{ m_groupActionMutex };
                m_groupWorker.request_stop();
                for (auto& entry : m_motors)
                    entry->stop();
            }
            if (m_groupWorker.joinable())
                m_groupWorker.join();
            motor.reset();
        }
        if (motor) {
            axis(*motor).stopAndWait();
        }
        else {
            // Stop every output before waiting for any worker to finish.
            for (auto& entry : m_motors)
                entry->stop();
            for (auto& entry : m_motors)
                entry->stopAndWait();
        }
        collect();
    }

    void MotionController::stop(std::optional<MotorId> motor)
    {
        std::scoped_lock lock{ m_mutex };
        stopLocked(motor);
    }

    void MotionController::disable()
    {
        std::scoped_lock lock{ m_mutex };
        m_enable->write(hal::gpio::Level::High);
        stopLocked(std::nullopt);
        for (auto& entry : m_motors)
            entry->invalidateReference();
    }

    void MotionController::reset()
    {
        std::scoped_lock lock{ m_mutex };
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before resetting the generator");
        stopLocked(std::nullopt);
        for (auto& entry : m_motors)
            entry->invalidateReference();
        static_cast<void>(m_generator->stop());
        if (const auto result{ m_generator->start() }; !result)
            throw std::runtime_error("step timebase restart failed: " + result.error().message());
        initializeDriversLocked();
    }

    void MotionController::collect()
    {
        for (auto& entry : m_motions) {
            if (!entry.result && entry.completion.valid() &&
                entry.completion.wait_for(0s) == std::future_status::ready) {
                try {
                    entry.result = entry.completion.get();
                } catch (...) {
                    entry.result = StepperMotor::Result::Faulted;
                }
            }
        }
    }

    void MotionController::reserveMotion(MotorId motor)
    {
        collect();
        if (m_motions.size() == 32U) {
            const auto completed{ std::ranges::find_if(
              m_motions, [](const auto& entry) { return entry.result.has_value(); }) };
            if (completed == m_motions.end())
                throw std::runtime_error("motion result capacity exhausted");
            m_motions.erase(completed);
        }
        // Allocate tracking before submitting anything that can move hardware.
        m_motions.push_back({ m_nextId++, motor, std::nullopt, {} });
    }

    MotionController::Motion MotionController::finishSubmission(std::future<StepperMotor::Result> future)
    {
        m_motions.back().completion = future.share();
        collect();
        return m_motions.back();
    }

    MotionController::Motion MotionController::move(MotorId motor, const Move& request)
    {
        std::scoped_lock lock{ m_mutex };
        return moveLocked(motor, request);
    }

    MotionController::Motion MotionController::moveLocked(MotorId motor, const Move& request)
    {
        requireManualAccess();
        auto& target{ axis(motor) };
        requireEnabled();
        if (request.absolute && !target.isReferenced())
            throw std::runtime_error("motor is not referenced; home this motor before an absolute move");
        reserveMotion(motor);
        try {
            return finishSubmission(request.absolute ? target.moveAbs(request.position,
                                                                      request.velocity,
                                                                      request.acceleration,
                                                                      request.deceleration,
                                                                      request.jerk,
                                                                      request.buffer,
                                                                      request.timeout)
                                                     : target.moveRel(request.position,
                                                                      request.velocity,
                                                                      request.acceleration,
                                                                      request.deceleration,
                                                                      request.jerk,
                                                                      request.buffer,
                                                                      request.timeout));
        } catch (...) {
            m_motions.pop_back();
            throw;
        }
    }

    MotionController::Motion MotionController::reference(MotorId motor,
                                                         pnm::units::AngularVelocity seek,
                                                         pnm::units::AngularVelocity latch,
                                                         pnm::units::Time timeout)
    {
        std::scoped_lock lock{ m_mutex };
        return referenceLocked(motor, seek, latch, timeout);
    }

    MotionController::Motion MotionController::referenceLocked(MotorId motor,
                                                               pnm::units::AngularVelocity seek,
                                                               pnm::units::AngularVelocity latch,
                                                               pnm::units::Time timeout)
    {
        requireManualAccess();
        auto& target{ axis(motor) };
        requireEnabled();
        reserveMotion(motor);
        try {
            return finishSubmission(target.reference(seek, latch, timeout));
        } catch (...) {
            m_motions.pop_back();
            throw;
        }
    }

    hal::step::PulseCount MotionController::setVelocity(MotorId motor, pnm::units::AngularVelocity velocity)
    {
        std::scoped_lock lock{ m_mutex };
        return setVelocityLocked(motor, velocity);
    }

    hal::step::PulseCount MotionController::setVelocityLocked(MotorId motor,
                                                              pnm::units::AngularVelocity velocity)
    {
        requireManualAccess();
        requireEnabled();
        const auto result{ axis(motor).setVelocity(velocity) };
        if (!result)
            throw std::runtime_error("velocity change unavailable: " + result.error().message());
        return *result;
    }

    StepperMotor::MotionDefaults MotionController::defaults(MotorId motor) const
    {
        std::scoped_lock lock{ m_mutex };
        return axis(motor).motionDefaults();
    }

    void MotionController::setDefaults(MotorId motor, StepperMotor::MotionDefaults defaults)
    {
        std::scoped_lock lock{ m_mutex };
        requireManualAccess();
        if (const auto result{ axis(motor).setMotionDefaults(defaults) }; !result)
            throw std::runtime_error("cannot set motion defaults: " + result.error().message());
    }

    MotionController::Status MotionController::status()
    {
        std::scoped_lock lock{ m_mutex };
        return statusLocked();
    }

    MotionController::Status MotionController::statusLocked()
    {
        const auto generator{ m_generator->status() };
        collect();
        Status status{ .enabled = m_enable->read() == hal::gpio::Level::Low,
                       .generator = generator,
                       .axes = {},
                       .coordinated = m_groupActive.load() };
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            const auto& motor{ *m_motors[i] };
            const auto id{ static_cast<MotorId>(i) };
            status.axes[i] = { id,
                               motor.position(),
                               motor.velocity(),
                               motor.actualPosition(),
                               motor.actualVelocity(),
                               motor.isReferenced(),
                               motor.referenceSwitchActive(),
                               static_cast<std::size_t>(std::ranges::count_if(
                                 m_motions,
                                 [id](const auto& entry) { return entry.motor == id && !entry.result; })),
                               m_conversions[i], motor.feedbackSource(), motor.feedbackResolution() };
        }
        return status;
    }

    std::vector<MotionController::Motion> MotionController::motions()
    {
        std::scoped_lock lock{ m_mutex };
        collect();
        return { m_motions.begin(), m_motions.end() };
    }

    namespace
    {
        template<typename T, typename... Alternatives>
        const T& typed(const std::variant<Alternatives...>& value)
        {
            if (const auto* result{ std::get_if<T>(&value) })
                return *result;
            throw std::invalid_argument("axis units do not match the configured rotary/linear axis");
        }

        template<typename Converter>
        using Speed = decltype(std::declval<Converter>().toAxisSpeed(0_rpm));

        template<typename Converter>
        using AxisRequest = std::conditional_t<std::is_same_v<Converter, RotaryAxisConversion>,
                                               MotionController::Move,
                                               MotionController::LinearMove>;

        template<typename Converter>
        using Defaults = std::conditional_t<std::is_same_v<Converter, RotaryAxisConversion>,
                                            StepperMotor::MotionDefaults,
                                            MotionController::LinearDefaults>;
    }

    MotionController::AxisConfig MotionController::motorConfiguration(MotorId motor) const
    {
        return m_configuration.at(static_cast<std::size_t>(motor));
    }

    void MotionController::configureAxis(MotorId motor, Conversion conversion)
    {
        std::scoped_lock lock{ m_mutex };
        requireManualAccess();
        const auto config{ motorConfiguration(motor) };
        if ((motor == MotorId::Motor3) != std::holds_alternative<LinearAxisConversion>(conversion))
            throw std::invalid_argument(
              "shoulder/elbow require rotary conversion; Z requires linear conversion");
        std::visit([&](const auto& value) {
            if (value.configuration().motor_reference != config.reference_position)
                throw std::invalid_argument(
                  "axis motor reference must match the motor's reference coordinate");
        }, conversion);
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before changing axis conversion");
        m_conversions[static_cast<std::size_t>(motor)] = std::move(conversion);
    }

    const MotionController::Conversion& MotionController::conversionLocked(MotorId motor) const
    {
        const auto& value{ m_conversions.at(static_cast<std::size_t>(motor)) };
        if (!value)
            throw std::runtime_error("axis mechanics are not configured in the application");
        return *value;
    }

    MotionController::Conversion MotionController::axisConversion(MotorId motor) const
    {
        std::scoped_lock lock{ m_mutex };
        return conversionLocked(motor);
    }

    MotionController::Motion MotionController::moveAxis(MotorId motor, const AxisMove& request)
    {
        std::scoped_lock lock{ m_mutex };
        return std::visit([&]<typename Converter>(const Converter& conversion) {
            const auto& value{ typed<AxisRequest<Converter>>(request) };
            const Move converted{
                .position = value.absolute ? conversion.toMotorPosition(value.position)
                                           : conversion.toMotorDisplacement(value.position),
                .velocity = conversion.toMotorSpeed(value.velocity),
                .acceleration = conversion.toMotorAcceleration(value.acceleration),
                .deceleration = conversion.toMotorAcceleration(value.deceleration),
                .jerk = conversion.toMotorJerk(value.jerk),
                .buffer = value.buffer,
                .timeout = value.timeout,
                .absolute = value.absolute,
            };
            return moveLocked(motor, converted);
        }, conversionLocked(motor));
    }

    MotionController::Motion MotionController::referenceAxis(MotorId motor,
                                                             std::optional<AxisSpeed> seek,
                                                             std::optional<AxisSpeed> latch,
                                                             pnm::units::Time timeout)
    {
        std::scoped_lock lock{ m_mutex };
        return std::visit([&]<typename Converter>(const Converter& conversion) {
            const auto seek_motor{ seek ? conversion.toMotorSpeed(typed<Speed<Converter>>(*seek)) : 5_rpm };
            const auto latch_motor{ latch ? conversion.toMotorSpeed(typed<Speed<Converter>>(*latch))
                                          : 0.5_rpm };
            if (latch_motor >= seek_motor)
                throw std::invalid_argument("latch speed must be slower than seek speed");
            return referenceLocked(motor, seek_motor, latch_motor, timeout);
        }, conversionLocked(motor));
    }

    hal::step::PulseCount MotionController::setAxisVelocity(MotorId motor, const AxisSpeed& velocity)
    {
        std::scoped_lock lock{ m_mutex };
        return std::visit([&]<typename Converter>(const Converter& conversion) {
            return setVelocityLocked(motor, conversion.toMotorSpeed(typed<Speed<Converter>>(velocity)));
        }, conversionLocked(motor));
    }

    MotionController::AxisDefaults MotionController::axisDefaults(MotorId motor) const
    {
        std::scoped_lock lock{ m_mutex };
        const auto defaults{ axis(motor).motionDefaults() };
        return std::visit([&]<typename Converter>(const Converter& conversion) -> AxisDefaults {
            return Defaults<Converter>{ conversion.toAxisAcceleration(defaults.acceleration),
                                        conversion.toAxisAcceleration(defaults.deceleration),
                                        conversion.toAxisJerk(defaults.jerk) };
        }, conversionLocked(motor));
    }

    void MotionController::setAxisDefaults(MotorId motor, const AxisDefaults& defaults)
    {
        std::scoped_lock lock{ m_mutex };
        requireManualAccess();
        const auto converted{ std::visit([&]<typename Converter>(const Converter& conversion) {
            const auto& value{ typed<Defaults<Converter>>(defaults) };
            return StepperMotor::MotionDefaults{ conversion.toMotorAcceleration(value.acceleration),
                                                 conversion.toMotorAcceleration(value.deceleration),
                                                 conversion.toMotorJerk(value.jerk) };
        }, conversionLocked(motor)) };
        if (const auto result{ axis(motor).setMotionDefaults(converted) }; !result)
            throw std::runtime_error("cannot set axis motion defaults: " + result.error().message());
    }

    void MotionController::requireManualAccess() const
    {
        if (m_groupActive.load())
            throw std::runtime_error("robot operation owns all axes; stop it before manual commands");
    }

    void MotionController::requireIdleGroup()
    {
        requireManualAccess();
        requireEnabled();
        collect();
        if (std::ranges::any_of(m_motions, [](const auto& motion) { return !motion.result; }))
            throw std::runtime_error("stop or finish individual motor/axis motions before a robot operation");
        if (m_groupWorker.joinable())
            m_groupWorker.join();
    }

    namespace
    {
        void validateGroupTimeout(pnm::units::Time timeout, bool homing)
        {
            const auto now{ std::chrono::steady_clock::now() };
            if (!timeout.isFinite() || timeout < 0_s || (homing && timeout == 0_s) ||
                timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2)
                throw std::invalid_argument("invalid robot operation timeout");
        }
    }

    MotionController::GroupMotion MotionController::coordinate(const Planner& planner,
                                                               pnm::units::Time timeout)
    {
        validateGroupTimeout(timeout, false);
        if (!planner)
            throw std::invalid_argument("coordinated move requires a planner");
        std::scoped_lock lock{ m_mutex };
        requireIdleGroup();
        const auto snapshot{ statusLocked() };
        if (!snapshot.generator.counts_exact ||
            std::ranges::any_of(snapshot.axes, [](const auto& axis) { return !axis.referenced; }))
            throw std::runtime_error("reference all axes before a robot move");
        std::array<StepperMotor::MotionDefaults, 3> defaults;
        for (std::size_t i{}; i < defaults.size(); ++i)
            defaults[i] = m_motors[i]->motionDefaults();
        auto plan{ planner(snapshot, defaults) };
        if (!plan.duration.isFinite() || plan.duration < 0_s)
            throw std::invalid_argument("invalid coordinated move duration");
        // Check every participating axis before changing any DIR or preparation.
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            if (bool(plan.sequences[i]) != plan.delays[i].has_value() ||
                (plan.sequences[i] && (plan.sequences[i]->count() == 0U || *plan.delays[i] < 5us ||
                                       *plan.delays[i] > 53'687'091'100ns)))
                throw std::invalid_argument("invalid coordinated axis schedule");
            if (plan.sequences[i] && plan.forward[i] && m_motors[i]->referenceSwitchActive())
                throw std::runtime_error("robot target moves an axis towards its active reference switch");
        }
        try {
            for (std::size_t i{}; i < m_motors.size(); ++i) {
                if (plan.sequences[i] && !m_motors[i]->prepareCoordinated(plan.forward[i], plan.sequences[i]))
                    throw std::runtime_error("cannot prepare coordinated motor motion");
            }
            return launchGroup(Operation::Move, std::move(plan), timeout);
        } catch (...) {
            for (auto& motor : m_motors)
                motor->stopAndWait();
            throw;
        }
    }

    MotionController::GroupMotion MotionController::referenceAll(pnm::units::Time timeout)
    {
        validateGroupTimeout(timeout, true);
        std::scoped_lock lock{ m_mutex };
        requireIdleGroup();
        return launchGroup(Operation::Reference, {}, timeout);
    }

    MotionController::GroupMotion MotionController::launchGroup(Operation operation,
                                                                CoordinatedPlan plan,
                                                                pnm::units::Time timeout)
    {
        if (m_groupMotions.size() == 32U)
            m_groupMotions.pop_front(); // All preceding group operations are complete.
        auto completion{ std::make_shared<std::promise<StepperMotor::Result>>() };
        GroupMotion result{ m_nextGroupId++, operation, plan.duration, {}, completion->get_future().share() };
        m_groupMotions.push_back(result);
        m_groupNotification.clear();
        m_groupActive.store(true);
        try {
            m_groupWorker = runtime::thread::create_jthread(
              { .name = "robot-motion", .stack_size = 16384U },
              [this, operation, plan = std::move(plan), timeout, completion](std::stop_token stop) mutable {
                runGroup(operation, std::move(plan), timeout, std::move(completion), stop);
            });
        } catch (...) {
            m_groupActive.store(false);
            m_groupMotions.pop_back();
            throw;
        }
        return result;
    }

    void MotionController::runGroup(Operation operation,
                                    CoordinatedPlan plan,
                                    pnm::units::Time timeout,
                                    std::shared_ptr<std::promise<StepperMotor::Result>> completion,
                                    std::stop_token stop) noexcept
    {
        using Result = StepperMotor::Result;
        using Clock = std::chrono::steady_clock;
        auto result{ Result::Faulted };
        const std::stop_callback cancellation{ stop, [this] { m_groupNotification.signal(); } };
        try {
            const auto deadline{ timeout == 0_s ? Clock::time_point::max()
                                                : Clock::now() + timeout.toChrono<Clock::duration>() };
            if (operation == Operation::Reference) {
                result = Result::Completed;
                std::array<bool, 3> referenced{};
                // Reference Z first, then the shoulder and relative elbow.
                for (const auto index : { 2U, 0U, 1U }) {
                    std::future<Result> homing;
                    {
                        std::scoped_lock action{ m_groupActionMutex };
                        if (stop.stop_requested()) {
                            result = Result::Stopped;
                            break;
                        }
                        const auto remaining{ deadline - Clock::now() };
                        if (remaining <= Clock::duration::zero()) {
                            result = Result::TimedOut;
                            break;
                        }
                        homing = m_motors[index]->reference(5_rpm, 0.5_rpm, pnm::units::Time{ remaining });
                    }
                    for (;;) {
                        const auto state{ m_generator->status() };
                        bool invalid{};
                        for (std::size_t i{}; i < referenced.size(); ++i)
                            invalid |= referenced[i] && !m_motors[i]->isReferenced();
                        if (stop.stop_requested()) {
                            result = Result::Stopped;
                            break;
                        }
                        if (!state.counts_exact || state.state != hal::step::State::Running || invalid) {
                            result = Result::Faulted;
                            break;
                        }
                        if (homing.wait_for(0s) == std::future_status::ready) {
                            result = homing.get();
                            if (result == Result::Completed && !m_motors[index]->isReferenced())
                                result = Result::Faulted;
                            break;
                        }
                        if (Clock::now() >= deadline) {
                            result = Result::TimedOut;
                            break;
                        }
                        static_cast<void>(m_groupNotification.waitUntil(deadline));
                    }
                    if (result != Result::Completed)
                        break;
                    referenced[index] = true;
                }
            }
            else {
                const bool moving{ std::ranges::any_of(plan.sequences,
                                                       [](const auto& p) { return bool(p); }) };
                bool started{};
                {
                    std::scoped_lock action{ m_groupActionMutex };
                    bool blocked{};
                    for (std::size_t i{}; i < m_motors.size(); ++i)
                        blocked |= !m_motors[i]->isReferenced() ||
                                   (plan.sequences[i] && m_motors[i]->coordinatedBlocked());
                    if (stop.stop_requested())
                        result = Result::Stopped;
                    else if (blocked)
                        result = Result::Rejected;
                    else if (!moving)
                        result = Result::Completed;
                    else {
                        started = bool(m_generator->startPrepared(plan.delays));
                        if (!started)
                            result = Result::Faulted;
                    }
                }
                while (started) {
                    const auto state{ m_generator->status() };
                    bool finished{ true }, blocked{}, invalid{};
                    for (std::size_t i{}; i < m_motors.size(); ++i) {
                        invalid |= !m_motors[i]->isReferenced();
                        if (!plan.sequences[i])
                            continue;
                        finished &= state.axes[i] == hal::step::State::Completed;
                        blocked |= m_motors[i]->coordinatedBlocked();
                        invalid |= state.axes[i] != hal::step::State::Running &&
                                   state.axes[i] != hal::step::State::Completed;
                    }
                    if (stop.stop_requested()) {
                        result = Result::Stopped;
                        break;
                    }
                    if (!state.counts_exact || state.state != hal::step::State::Running || invalid) {
                        result = Result::Faulted;
                        break;
                    }
                    if (blocked) {
                        result = Result::Stopped;
                        break;
                    }
                    if (finished) {
                        result = Result::Completed;
                        break;
                    }
                    if (Clock::now() >= deadline) {
                        result = Result::TimedOut;
                        break;
                    }
                    static_cast<void>(m_groupNotification.waitUntil(deadline));
                }
            }
        } catch (...) {
            result = Result::Faulted;
        }
        for (auto& motor : m_motors)
            motor->stop();
        for (auto& motor : m_motors)
            motor->stopAndWait();
        if (!m_generator->status().counts_exact)
            result = Result::Faulted;
        else if (stop.stop_requested() && result == Result::Completed)
            result = Result::Stopped;
        // No further hardware access after releasing ownership. A new group
        // joins this worker before replacing it; retained futures stay valid.
        m_groupActive.store(false);
        completion->set_value(result);
    }

    std::vector<MotionController::GroupMotion> MotionController::groupMotions()
    {
        std::scoped_lock lock{ m_mutex };
        for (auto& entry : m_groupMotions) {
            if (!entry.result && entry.completion.wait_for(0s) == std::future_status::ready)
                entry.result = entry.completion.get();
        }
        return { m_groupMotions.begin(), m_groupMotions.end() };
    }
}
