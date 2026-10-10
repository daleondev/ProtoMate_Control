#include "MotionController.hpp"
#include "MotionLimits.hpp"
#include "SynchronizedSequence.hpp"
#include "pneumo/pneumo.hpp"
#include "runtime/thread.hpp"

#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#endif

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace
{
    constexpr int DRIVER_MONITOR_PRIORITY{ 18 };
    constexpr std::size_t DRIVER_MONITOR_STACK_SIZE{ 8192U };
    constexpr std::size_t GROUP_WORKER_STACK_SIZE{ 16384U };
}

namespace control
{
    using namespace std::chrono_literals;

    MotionController::MotionController(const std::array<AxisConfig, 3>& configuration,
                                       const DriverFactory& driver_factory)
      : m_configuration{ configuration }
      , m_enable{ hal::board::createSteppersEnableOutput() }
      , m_generator{ hal::board::createStepperGenerator() }
    {
        if (!m_enable || !m_generator)
            throw std::runtime_error("step generator/enable creation failed");
        m_enable->write(hal::gpio::Level::High);
        try {
            const std::array microsteps{ configuration[0].microsteps,
                                         configuration[1].microsteps,
                                         configuration[2].microsteps };
            auto drivers{ driver_factory ? driver_factory(microsteps, m_enable)
                                         : hal::board::createStepperDrivers(microsteps, m_enable) };
            for (std::size_t i{}; i < m_motors.size(); ++i) {
                const auto& config{ configuration[i] };
                m_motors[i] = std::make_unique<StepperMotor>(
                  static_cast<MotorId>(i),
                  config.reference_position,
                  config.full_step_angle,
                  config.microsteps,
                  m_generator,
                  std::move(drivers[i]),
                  hal::board::createMotorFeedback(
                    static_cast<MotorId>(i),
                    { .full_step_angle = config.full_step_angle, .microsteps = config.microsteps }),
                  [this, i]() noexcept {
                    m_driverFaults.fetch_or(1U << i, std::memory_order_release);
                    m_enable->write(hal::gpio::Level::High);
                    m_driverNotification.signal();
                });
                m_motors[i]->m_events->group_notification.store(&m_groupNotification);
            }
            if (const auto result{ m_generator->start() }; !result)
                throw std::runtime_error("step timebase start failed: " + result.error().message());
            // Missing/unpowered drivers leave the console usable for diagnosis.
            try {
                initializeDriversLocked();
            } catch (const std::exception& e) {
                pnm::log::error("Stepper driver initialization: {}", e.what());
            }
            m_driverWorker =
              runtime::thread::create_jthread({ .name = "Driver monitor",
                                                .priority = DRIVER_MONITOR_PRIORITY,
                                                .stack_size = DRIVER_MONITOR_STACK_SIZE },
                                              [this](const std::stop_token& stop) { monitorDrivers(stop); });
        } catch (...) {
            // Detach before member unwinding destroys callback destinations.
            for (auto& motor : m_motors)
                if (motor)
                    motor->driver().clearFaultCallback();
            m_enable->write(hal::gpio::Level::High);
            static_cast<void>(m_generator->stop());
            throw;
        }
    }

    MotionController::~MotionController()
    {
        try {
            for (auto& motor : m_motors)
                motor->driver().clearFaultCallback();
            static_cast<void>(m_driverWorker.request_stop());
            m_driverNotification.signal();
            if (m_driverWorker.joinable())
                m_driverWorker.join();
            disable();
            static_cast<void>(m_generator->stop());
        }

        catch (...) {
            // Cleanup failure must never leave a powered axis running.
            m_enable->write(hal::gpio::Level::High);
            std::terminate();
        }
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
        if (m_remoteInhibited.load())
            throw std::runtime_error("communication lost; reset before enabling remote motion");
        if (!driversReady() || m_driverFaults.load(std::memory_order_acquire))
            throw std::runtime_error("stepper driver fault/not initialized; inspect 'motor driver status' "
                                     "then reset while disabled");
        if (m_enable->read() != hal::gpio::Level::Low)
            throw std::runtime_error("drivers disabled; enable drivers before requesting motion");
        if (m_generator->status().state != hal::step::State::Running)
            throw std::runtime_error("step generator fault/stopped; disable, reset and re-reference");
    }

    void MotionController::enable()
    {
        std::scoped_lock lock{ m_mutex };
        if (m_remoteInhibited.load())
            throw std::runtime_error("communication lost; disable/reset before enabling");
        if (m_generator->status().state != hal::step::State::Running)
            throw std::runtime_error("step generator fault/stopped; disable drivers and reset the timebase");
        bool failed{};
        for (auto& motor : m_motors)
            failed |= !motor->driver().verify();
        handleDriverFaultsLocked();
        if (failed || !driversReady() || m_driverFaults.load())
            throw std::runtime_error(
              "drivers not ready; inspect 'motor driver status' and run 'motor driver init'");
        if (m_enable->read() == hal::gpio::Level::Low)
            return;
        bool permitted{};
        {
#ifdef HAL_PLATFORM_STM32
            // A fault IRQ cannot be overwritten by the enable write.
            const hal::stm32::InterruptGuard guard;
#endif
            permitted = driversReady() && m_driverFaults.load() == 0;
            if (permitted)
                m_enable->write(hal::gpio::Level::Low);
        }
        // Also closes the host simulation's concurrent-callback window.
        if (!permitted || m_driverFaults.load() || !driversReady()) {
            m_enable->write(hal::gpio::Level::High);
            handleDriverFaultsLocked();
            throw std::runtime_error("driver fault before enable");
        }
        std::this_thread::sleep_for(200ms);
        if (!driversReady() || m_driverFaults.load() || m_enable->read() != hal::gpio::Level::Low) {
            m_enable->write(hal::gpio::Level::High);
            handleDriverFaultsLocked();
            throw std::runtime_error("driver fault during enable settling");
        }
    }

    void MotionController::stopLocked(std::optional<MotorId> motor)
    {
        ++m_commandRevision;
        if (motor)
            static_cast<void>(axis(*motor));
        if (m_groupActive.load()) {
            // Serialize cancellation with the worker's arm/reference submission.
            // Release this lock before joining: the worker never takes m_mutex.
            {
                std::scoped_lock action{ m_groupActionMutex };
                static_cast<void>(m_groupWorker.request_stop());
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
        ++m_commandRevision;
        collect();
        if (m_motions.size() == MOTION_HISTORY_CAPACITY) {
            const auto completed{ std::ranges::find_if(
              m_motions, [](const auto& entry) { return entry.result.has_value(); }) };
            if (completed == m_motions.end())
                throw std::runtime_error("motion result capacity exhausted");
            m_motions.erase(completed);
        }
        // Allocate tracking before submitting anything that can move hardware.
        m_motions.push_back({ m_nextId++, motor, std::nullopt, {}, {} });
    }

    MotionController::Motion MotionController::finishSubmission(std::future<StepperMotor::Result> future)
    {
        m_motions.back().completion = future.share();
        m_motions.back().observed = axis(m_motions.back().motor).m_lastCompletion;
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
        ++m_commandRevision;
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
        ++m_configurationRevision;
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
                       .coordinated = m_groupActive.load(),
                       .configuration_revision = m_configurationRevision,
                       .command_revision = m_commandRevision };
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            const auto& motor{ *m_motors[i] };
            const auto id{ static_cast<MotorId>(i) };
            status.axes[i] = { .motor = id,
                               .position = motor.position(),
                               .velocity = motor.velocity(),
                               .actual_position = motor.actualPosition(),
                               .actual_velocity = motor.actualVelocity(),
                               .referenced = motor.isReferenced(),
                               .reference_switch_active = motor.referenceSwitchActive(),
                               .outstanding = static_cast<std::size_t>(std::ranges::count_if(
                                 m_motions,
                                 [id](const auto& entry) { return entry.motor == id && !entry.result; })),
                               .conversion = m_conversions[i],
                               .feedback_source = motor.feedbackSource(),
                               .feedback_resolution = motor.feedbackResolution() };
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
        ++m_configurationRevision;
        m_conversions[static_cast<std::size_t>(motor)] = conversion;
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
            const auto seek_motor{ seek ? conversion.toMotorSpeed(typed<Speed<Converter>>(*seek))
                                        : control::DEFAULT_REFERENCE_SEEK };
            const auto latch_motor{ latch ? conversion.toMotorSpeed(typed<Speed<Converter>>(*latch))
                                          : control::DEFAULT_REFERENCE_LATCH };
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
        ++m_configurationRevision;
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
        void validate_group_timeout(pnm::units::Time timeout, bool homing)
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
        validate_group_timeout(timeout, false);
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
            const auto delay{ plan.delays[i] };
            if (bool(plan.sequences[i]) != delay.has_value())
                throw std::invalid_argument("invalid coordinated axis schedule");
            if (plan.sequences[i] &&
                (!delay || plan.sequences[i]->count() == 0U || *delay < motion::limits::HIGH_TIME ||
                 *delay > motion::limits::MAX_PERIOD))
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
        validate_group_timeout(timeout, true);
        std::scoped_lock lock{ m_mutex };
        requireIdleGroup();
        return launchGroup(Operation::Reference, {}, timeout);
    }

    MotionController::GroupMotion MotionController::launchGroup(Operation operation,
                                                                CoordinatedPlan plan,
                                                                pnm::units::Time timeout)
    {
        ++m_commandRevision;
        if (m_groupMotions.size() == MOTION_HISTORY_CAPACITY)
            m_groupMotions.pop_front(); // All preceding group operations are complete.
        Completion<MotorResult> completion;
        GroupMotion result{ .id = m_nextGroupId++,
                            .operation = operation,
                            .duration = plan.duration,
                            .result = {},
                            .completion = completion.getFuture().share(),
                            .observed = completion };
        m_groupMotions.push_back(result);
        m_groupNotification.clear();
        m_groupActive.store(true);
        try {
            m_groupWorker = runtime::thread::create_jthread(
              { .name = "robot-motion", .stack_size = GROUP_WORKER_STACK_SIZE },
              [this, operation, plan = std::move(plan), timeout, completion](
                const std::stop_token& stop) mutable {
                runGroup(operation, plan, timeout, std::move(completion), stop);
            });
        } catch (...) {
            m_groupActive.store(false);
            m_groupMotions.pop_back();
            throw;
        }
        return result;
    }

    MotorResult MotionController::waitForReference(std::size_t index,
                                                   std::future<MotorResult>& homing,
                                                   const std::array<bool, 3>& referenced,
                                                   std::chrono::steady_clock::time_point deadline,
                                                   const std::stop_token& stop)
    {
        using Result = MotorResult;
        using Clock = std::chrono::steady_clock;
        auto result{ Result::Faulted };
        for (;;) {
            const auto state{ m_generator->status() };
            bool invalid{};
            for (std::size_t i{}; i < referenced.size(); ++i)
                invalid |= referenced[i] && !m_motors[i]->isReferenced();
            if (stop.stop_requested()) {
                result = Result::Stopped;
                break;
            }
            if (!driversReady() || m_driverFaults.load() || !state.counts_exact ||
                state.state != hal::step::State::Running || invalid) {
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
        return result;
    }
    MotorResult MotionController::referenceGroup(std::chrono::steady_clock::time_point deadline,
                                                 const std::stop_token& stop)
    {
        using Result = MotorResult;
        using Clock = std::chrono::steady_clock;
        auto result{ Result::Completed };
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
                homing = m_motors[index]->reference(control::DEFAULT_REFERENCE_SEEK,
                                                    control::DEFAULT_REFERENCE_LATCH,
                                                    pnm::units::Time{ remaining });
            }
            result = waitForReference(index, homing, referenced, deadline, stop);
            if (result != Result::Completed)
                break;
            referenced[index] = true;
        }
        return result;
    }
    MotorResult MotionController::moveGroup(const CoordinatedPlan& plan,
                                            std::chrono::steady_clock::time_point deadline,
                                            const std::stop_token& stop)
    {
        using Result = MotorResult;
        auto result{ Result::Faulted };
        const bool moving{ std::ranges::any_of(plan.sequences, [](const auto& p) { return bool(p); }) };
        bool started{};
        {
            std::scoped_lock action{ m_groupActionMutex };
            bool blocked{ !driversReady() || m_driverFaults.load() != 0 };
            for (std::size_t i{}; i < m_motors.size(); ++i)
                blocked |=
                  !m_motors[i]->isReferenced() || (plan.sequences[i] && m_motors[i]->coordinatedBlocked());
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
        return started ? waitForGroup(plan, deadline, stop) : result;
    }
    MotorResult MotionController::waitForGroup(const CoordinatedPlan& plan,
                                               std::chrono::steady_clock::time_point deadline,
                                               const std::stop_token& stop)
    {
        using Result = MotorResult;
        using Clock = std::chrono::steady_clock;
        auto result{ Result::Faulted };
        while (true) {
            const auto state{ m_generator->status() };
            bool finished{ true };
            bool blocked{};
            bool invalid{};
            for (std::size_t i{}; i < m_motors.size(); ++i) {
                invalid |= !m_motors[i]->isReferenced();
                if (!plan.sequences[i])
                    continue;
                finished &= state.axes[i] == hal::step::State::Completed;
                blocked |= m_motors[i]->coordinatedBlocked();
                invalid |=
                  state.axes[i] != hal::step::State::Running && state.axes[i] != hal::step::State::Completed;
            }
            if (stop.stop_requested()) {
                result = Result::Stopped;
                break;
            }
            if (!driversReady() || m_driverFaults.load() || !state.counts_exact ||
                state.state != hal::step::State::Running || invalid) {
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
        return result;
    }
    void MotionController::runGroup(Operation operation,
                                    const CoordinatedPlan& plan,
                                    pnm::units::Time timeout,
                                    Completion<MotorResult> completion,
                                    const std::stop_token& stop) noexcept
    {
        using Result = StepperMotor::Result;
        using Clock = std::chrono::steady_clock;
        auto result{ Result::Faulted };
        const std::stop_callback cancellation{ stop, [this] { m_groupNotification.signal(); } };
        try {
            const auto deadline{ timeout == 0_s ? Clock::time_point::max()
                                                : Clock::now() + timeout.toChrono<Clock::duration>() };
            result = operation == Operation::Reference ? referenceGroup(deadline, stop)
                                                       : moveGroup(plan, deadline, stop);
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
        completion.setValue(result);
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

namespace control
{
    MotionController::PlanningSnapshot MotionController::planningSnapshot()
    {
        std::scoped_lock lock(m_mutex);
        PlanningSnapshot result{ .status = statusLocked(), .defaults = {}, .configuration = m_configuration };
        for (unsigned i = 0; i < 3; ++i)
            result.defaults[i] = m_motors[i]->motionDefaults();
        return result;
    }
    namespace
    {
        double peak_velocity(const auto& phase)
        {
            const auto velocity = [&](double t) {
                return phase.velocity + (phase.acceleration * t) + (phase.jerk * t * t / 2);
            };
            auto peak = std::max(velocity(0), velocity(phase.duration));
            if (phase.jerk != 0) {
                auto t = -phase.acceleration / phase.jerk;
                if (t > 0 && t < phase.duration)
                    peak = std::max(peak, velocity(t));
            }
            return peak;
        }
        void validate_acceleration_continuity(const motion::Profile::Description& description,
                                              unsigned p,
                                              const MotionDefaults& defaults,
                                              double distance)
        {
            const auto& phase{ description.phases[p] };
            if (defaults.jerk.get() > 0) {
                const auto previous =
                  p ? description.phases[p - 1].acceleration +
                        (description.phases[p - 1].jerk * description.phases[p - 1].duration)
                    : 0;
                if (std::abs(phase.acceleration - previous) * distance > motion::limits::DYNAMICS_TOLERANCE ||
                    (p + 1 == description.size &&
                     std::abs(phase.acceleration + (phase.jerk * phase.duration)) * distance >
                       motion::limits::DYNAMICS_TOLERANCE))
                    throw std::invalid_argument("discontinuous acceleration in jerk-limited trajectory");
            }
        }
        void validate_axis_profile(const motion::Profile::Description& description,
                                   hal::step::PulseCount count,
                                   const MotionDefaults& defaults,
                                   double distance)
        {
            for (unsigned p = 0; p < description.size; ++p) {
                const auto& phase = description.phases[p];
                const auto a0 = phase.acceleration;
                const auto a1 = a0 + (phase.jerk * phase.duration);
                const auto peak{ peak_velocity(phase) };
                if (peak * static_cast<double>(count) >
                    motion::limits::MAX_PLANNED_RATE * (1 + motion::limits::RATE_TOLERANCE))
                    throw std::invalid_argument("trajectory exceeds STEP frequency limit");
                validate_acceleration_continuity(description, p, defaults, distance);
                const auto allowed = [&](double a) {
                    return a >= 0 ? defaults.acceleration.get() : defaults.deceleration.get();
                };
                if (std::abs(a0) * distance > allowed(a0) * (1 + motion::limits::DYNAMICS_TOLERANCE) ||
                    std::abs(a1) * distance > allowed(a1) * (1 + motion::limits::DYNAMICS_TOLERANCE) ||
                    (defaults.jerk.get() > 0 &&
                     std::abs(phase.jerk) * distance >
                       defaults.jerk.get() * (1 + motion::limits::DYNAMICS_TOLERANCE)))
                    throw std::invalid_argument("trajectory exceeds configured dynamics");
            }
        }
    }
    MotionController::GroupMotion MotionController::coordinate(const Trajectory& trajectory,
                                                               pnm::units::Time timeout)
    {
        return coordinate([&](const Status& state, const std::array<MotionDefaults, 3>& defaults) {
            return planTrajectory(trajectory, state, defaults);
        }, timeout);
    }
    MotionController::CoordinatedPlan MotionController::planTrajectory(
      const Trajectory& trajectory,
      const Status& state,
      const std::array<MotionDefaults, 3>& defaults)
    {
        if (trajectory.configuration_revision != m_configurationRevision ||
            trajectory.command_revision != m_commandRevision)
            throw std::runtime_error("stale robot plan; request a fresh planning snapshot");
        for (unsigned i = 0; i < 3; ++i)
            if (trajectory.origin[i] != state.axes[i].position)
                throw std::runtime_error("robot plan origin changed");
        const auto profile = motion::Profile::fromDescription(trajectory.profile);
        if (!profile)
            throw std::invalid_argument("invalid serialized motion profile");
        CoordinatedPlan plan;
        plan.forward = trajectory.forward;
        plan.duration = trajectory.duration;
        bool moving = std::ranges::any_of(trajectory.pulses, [](auto n) { return n != 0; });
        if (!moving) {
            if (trajectory.profile.size || trajectory.duration != 0_s)
                throw std::invalid_argument("invalid empty plan");
            return plan;
        }
        if (std::abs(profile->distance() - 1.0) > motion::limits::NORMALIZATION_TOLERANCE ||
            std::abs(profile->endVelocity()) > motion::limits::NORMALIZATION_TOLERANCE ||
            std::abs(trajectory.duration.get() -
                     (profile->duration() +
                      std::chrono::duration<double>{ motion::limits::COORDINATED_DURATION_MARGIN }.count())) >
              motion::limits::NORMALIZATION_TOLERANCE)
            throw std::invalid_argument("coordinated profile must be normalized and end at rest");
        for (unsigned i = 0; i < 3; ++i) {
            auto count = trajectory.pulses[i];
            if (!count)
                continue;
            if (count >= motion::limits::MAX_PULSES)
                throw std::invalid_argument("pulse count out of range");
            const auto distance = m_configuration[i].full_step_angle.get() /
                                  static_cast<double>(m_configuration[i].microsteps) *
                                  static_cast<double>(count);
            validate_axis_profile(trajectory.profile, count, defaults[i], distance);
            auto sequence = std::make_shared<motion::SynchronizedSequence>(*profile, count);
            plan.delays[i] = sequence->firstDelay() + motion::limits::COORDINATED_START_DELAY;
            plan.sequences[i] = std::move(sequence);
        }
        return plan;
    }

    void MotionController::inhibitRemoteMotion() noexcept
    {
        m_remoteInhibited.store(true, std::memory_order_release);
        // Leaves driver holding torque intact. Stopping the timebase also prevents
        // an in-flight command worker from arming another output after link loss.
        static_cast<void>(m_generator->stop());
        m_groupNotification.signal();
        for (auto& motor : m_motors)
            motor->m_events->notification.signal();
    }
    void MotionController::recoverRemoteMotion()
    {
        reset();
        m_remoteInhibited.store(false, std::memory_order_release);
    }
}

namespace control
{
    void MotionController::requestStop(std::optional<MotorId> motor) noexcept
    {
        if (m_groupActive.load())
            motor.reset();
        if (motor && static_cast<unsigned>(*motor) < 3)
            m_motors[static_cast<unsigned>(*motor)]->requestStop();
        else
            for (auto& entry : m_motors)
                entry->requestStop();
        m_groupNotification.signal();
    }
}

namespace control
{
    void MotionController::cancelMotion(MotionId id, bool group)
    {
        std::scoped_lock lock(m_mutex);
        if (group) {
            auto entry = std::ranges::find_if(m_groupMotions, [&](const auto& v) { return v.id == id; });
            if (entry != m_groupMotions.end() && !entry->observed.result())
                stopLocked(std::nullopt);
            return;
        }
        auto entry = std::ranges::find_if(m_motions, [&](const auto& v) { return v.id == id; });
        if (entry == m_motions.end() || entry->observed.result())
            return;
        auto& target = axis(entry->motor);
        {
            std::scoped_lock queue(target.m_queueMutex);
            if (target.m_pending && target.m_pending->completion == entry->observed) {
                target.m_pending->completion.setValue(MotorResult::Stopped);
                target.m_pending.reset();
                ++m_commandRevision;
                return;
            }
        }
        // A successor already committed to a blended pulse schedule requires an
        // axis stop; stopping it cannot safely preserve the predecessor's schedule.
        stopLocked(entry->motor);
    }
}
