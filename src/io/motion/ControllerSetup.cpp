#include "Controller.hpp"
#include "Limits.hpp"
#include "profiling/Trace.h"
#include <cmath>
#include <stdexcept>
#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#endif
namespace io::motion
{
    namespace
    {
        constexpr std::uint8_t ALL_AXES{ 7 };
        constexpr double MAX_ACCELERATION{ 1e8 }, MAX_JERK{ 1e10 };
        constexpr auto ENABLE_SETTLING_TIME{ std::chrono::milliseconds(200) };
        std::future<StepperMotor::Result> rejected_motion(const std::function<void()>& notify)
        {
            std::promise<StepperMotor::Result> completion{};
            completion.set_value(StepperMotor::Result::Rejected);
            if (notify) {
                notify();
            }
            return completion.get_future();
        }
    }
    void Controller::initializeCapabilities()
    {
        namespace tr = ::motion::trajectory;
        m_capabilities.reference_forward_mask = 0;
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto step{ m_configuration[i].full_step_angle.get() /
                             static_cast<double>(m_configuration[i].microsteps) };
            if (m_configuration[i].reference_direction == StepperMotor::Direction::Forward) {
                m_capabilities.reference_forward_mask |= 1U << i;
            }
            m_capabilities.step_radians[i] = step;
            m_capabilities.reference_steps[i] = m_configuration[i].reference_position.get() / step;
            m_capabilities.limits[i] = { .minimum = -tr::MAX_POSITION,
                                         .maximum = tr::MAX_POSITION,
                                         .velocity = Executor::MAX_PRODUCTION_RATE,
                                         .acceleration = MAX_ACCELERATION,
                                         .deceleration = MAX_ACCELERATION,
                                         .jerk = MAX_JERK };
        }
        m_capabilities.coordinated_execution = true;
        m_capabilities.hardware_fingerprint = tr::fingerprint(m_capabilities);
    }
    ::motion::trajectory::Reply Controller::trajectorySnapshot() const
    {
        std::scoped_lock lock{ m_mutex };
        return trajectorySnapshotLocked();
    }

    ::motion::trajectory::Reply Controller::trajectorySnapshotLocked() const
    {
        namespace tr = ::motion::trajectory;
        tr::Reply result{};
        result.capabilities = m_capabilities;
        result.boot = m_boot;
        result.family = m_ownership.family();
        result.owners = m_ownership.owners();
        result.axis_revisions = m_ownership.revisions();
        result.revision = m_controlEpoch;
        result.enabled = enabled();
        const auto generator{ m_generator->status() };
        result.counts_exact = generator.counts_exact;
        result.state = result.enabled ? tr::State::Unreferenced : tr::State::Disabled;
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto step{ m_capabilities.step_radians[i] };
            const auto count{ tr::quantize(m_motors[i]->setpointPosition().get() / step) };
            if (count) {
                result.position[i] = *count;
            }
            else {
                result.counts_exact = false;
            }
            result.boundary[i].position = static_cast<double>(result.position[i]);
            result.boundary[i].velocity = m_motors[i]->setpointVelocity().get() / step;
            if (m_motors[i]->isReferenced()) {
                result.referenced |= static_cast<std::uint8_t>(1U << i);
            }
        }
        if (result.enabled && result.referenced == ALL_AXES) {
            result.state = tr::State::Ready;
        }
        if (m_faulted.load() || !result.counts_exact) {
            result.state = tr::State::Faulted;
        }
        for (const auto& group : m_groups) {
            if (!result.enabled || !result.counts_exact || !group.executor) {
                continue;
            }
            switch (group.executor->status().state) {
                case Executor::State::Prepared:
                    result.state = tr::State::Staged;
                    break;
                case Executor::State::Running:
                    result.state = tr::State::Running;
                    break;
                case Executor::State::Braking:
                    result.state = tr::State::Holding;
                    break;
                case Executor::State::Faulted:
                    result.state = tr::State::Faulted;
                    break;
                default:
                    break;
            }
        }
        return result;
    }

    void Controller::initializeDrivers(const std::stop_token& stop)
    {
        std::scoped_lock operation{ m_operationMutex };
        std::uint64_t epoch{};
        {
            std::scoped_lock lock{ m_mutex };
            if (stop.stop_requested()) {
                throw std::runtime_error("driver initialization cancelled");
            }
            if (m_enable->read() != hal::gpio::Level::High) {
                throw std::logic_error("disable motors before initializing drivers");
            }
            epoch = m_controlEpoch;
            m_initialized = false;
            stopLocked();
            invalidateReferencesLocked();
            static_cast<void>(m_generator->stop());
            m_faulted.store(false);
            m_faultHandled = false;
        }
        bool failed{};
        for (auto& motor : m_motors) {
            // UART may block. Stop/disable must still acquire m_mutex meanwhile.
            failed |= !motor->driver().initialize();
            std::scoped_lock lock{ m_mutex };
            if (stop.stop_requested() || epoch != m_controlEpoch) {
                throw std::runtime_error("driver initialization cancelled");
            }
        }
        std::scoped_lock lock{ m_mutex };
        if (stop.stop_requested() || epoch != m_controlEpoch) {
            throw std::runtime_error("driver initialization cancelled");
        }
        handleFaultsLocked();
        if (failed || m_faulted.load() || !driversReady()) {
            throw std::runtime_error("driver initialization failed; inspect motor driver status");
        }
        if (const auto result{ m_generator->start() }; !result) {
            throw std::runtime_error("STEP timebase start failed: " + result.error().message());
        }
        m_initialized = true;
        m_admissionLost = false;
        for (auto& g : m_groups) {
            if (g.executor && !g.executor->recover()) {
                throw std::runtime_error("executor recovery failed");
            }
        }
    }

    void Controller::verifyEnable(std::uint64_t epoch, const std::stop_token& stop)
    {
        for (auto& motor : m_motors) {
            const bool verified{ motor->driver().verify().has_value() };
            std::scoped_lock lock{ m_mutex };
            if (stop.stop_requested() || epoch != m_controlEpoch) {
                throw std::runtime_error("motor enable cancelled");
            }
            if (!verified) {
                m_faulted.store(true);
            }
        }
    }
    hal::util::Result<std::uint8_t> Controller::validateExecutionLocked(
      const Executor::Command& command) const
    {
        const auto fail{ [](std::errc e) { return std::unexpected(std::make_error_code(e)); } };
        std::uint8_t mask{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto& a{ command.axes[i] };
            if (!a.profile) {
                continue;
            }
            mask |= 1U << i;
            if (!std::isfinite(a.limits.velocity) || a.limits.velocity > Executor::MAX_PRODUCTION_RATE) {
                return fail(std::errc::invalid_argument);
            }
            const auto& motor{ *m_motors[i] };
            std::scoped_lock queue{ motor.m_queueMutex };
            if (motor.m_busy || motor.m_homing || motor.m_pending) {
                return fail(std::errc::device_or_resource_busy);
            }
            if (!motor.isReferenced() || motor.referenceSwitchActive()) {
                return fail(std::errc::operation_not_permitted);
            }
            for (const auto& g : m_groups) {
                if ((g.mask & (1U << i)) && g.executor && g.executor->reserved()) {
                    return fail(std::errc::device_or_resource_busy);
                }
            }
        }
        return mask;
    }
    void Controller::enable(const std::stop_token& stop)
    {
        profile_allow_arm(0);
        std::scoped_lock operation{ m_operationMutex };
        const std::stop_callback cancellation{ stop, [this] { m_controlNotification.signal(); } };
        std::uint64_t epoch{};
        {
            std::scoped_lock lock{ m_mutex };
            if (stop.stop_requested()) {
                throw std::runtime_error("motor enable cancelled");
            }
            handleFaultsLocked();
            if (!m_initialized || m_faulted.load() || !driversReady()) {
                throw std::runtime_error("initialize all drivers before enabling motors");
            }
            if (enabled()) {
                return;
            }
            epoch = m_controlEpoch;
            m_enabling = true;
        }
        try {
            verifyEnable(epoch, stop);
            {
                std::scoped_lock lock{ m_mutex };
                if (stop.stop_requested() || epoch != m_controlEpoch) {
                    throw std::runtime_error("motor enable cancelled");
                }
                handleFaultsLocked();
                bool permitted{};
                {
#ifdef HAL_PLATFORM_STM32
                    // Do not overwrite an alarm ISR's disable write with our enable.
                    const hal::stm32::InterruptGuard guard{};
#endif
                    permitted = !m_faulted.load() && driversReady() &&
                                std::ranges::none_of(
                                  m_motors, [](const auto& motor) { return motor->referenceSwitchActive(); });
                    if (permitted) {
                        m_enable->write(hal::gpio::Level::Low);
                    }
                }
                if (!permitted || !enabled() || !driversReady()) {
                    m_faulted.store(true);
                    handleFaultsLocked();
                    throw std::runtime_error(
                      "enable rejected: driver fault or active limit switch; clear switches before homing");
                }
            }
            const auto deadline{ std::chrono::steady_clock::now() + ENABLE_SETTLING_TIME };
            for (;;) {
                {
                    std::scoped_lock lock{ m_mutex };
                    if (stop.stop_requested() || epoch != m_controlEpoch) {
                        throw std::runtime_error("motor enable cancelled");
                    }
                    if (!enabled() || !driversReady()) {
                        m_faulted.store(true);
                        handleFaultsLocked();
                        throw std::runtime_error("driver fault during enable settling");
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        m_enabling = false;
                        return;
                    }
                }
                m_controlNotification.waitUntil(deadline);
            }
        } catch (...) {
            std::scoped_lock lock{ m_mutex };
            m_enable->write(hal::gpio::Level::High);
            m_enabling = false;
            throw;
        }
    }
    pnm::Result<> Controller::setMotionDefaults(MotorId id, Defaults defaults)
    {
        std::scoped_lock lock{ m_mutex };
        if (m_ownership.family() != ::motion::Family::None) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        auto result{ axis(id).setMotionDefaults(defaults) };
        if (result) {
            ++m_controlEpoch;
            m_ownership.changed(1U << unsigned(id));
        }
        return result;
    }

    std::future<StepperMotor::Result> Controller::moveRel(MotorId id,
                                                          pnm::units::Angle distance,
                                                          pnm::units::AngularVelocity velocity,
                                                          pnm::units::AngularAcceleration acceleration,
                                                          pnm::units::AngularAcceleration deceleration,
                                                          pnm::units::AngularJerk jerk,
                                                          BufferMode buffer_mode,
                                                          pnm::units::Time timeout,
                                                          const std::function<void()>& completed)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked() || !velocityAllowed(id, velocity)) {
            return rejected_motion(completed);
        }
        auto future{ target.moveRel(
          distance, velocity, acceleration, deceleration, jerk, buffer_mode, timeout) };
        target.m_lastCompletion.then(completed);
        return future;
    }

    std::future<StepperMotor::Result> Controller::moveAbs(MotorId id,
                                                          pnm::units::Angle position,
                                                          pnm::units::AngularVelocity velocity,
                                                          pnm::units::AngularAcceleration acceleration,
                                                          pnm::units::AngularAcceleration deceleration,
                                                          pnm::units::AngularJerk jerk,
                                                          BufferMode buffer_mode,
                                                          pnm::units::Time timeout,
                                                          const std::function<void()>& completed)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked() || !velocityAllowed(id, velocity)) {
            return rejected_motion(completed);
        }
        auto future{ target.moveAbs(
          position, velocity, acceleration, deceleration, jerk, buffer_mode, timeout) };
        target.m_lastCompletion.then(completed);
        return future;
    }

    std::future<StepperMotor::Result> Controller::reference(MotorId id,
                                                            pnm::units::AngularVelocity seek_velocity,
                                                            pnm::units::AngularVelocity latch_velocity,
                                                            pnm::units::Time timeout,
                                                            const std::function<void()>& completed)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked() || !velocityAllowed(id, seek_velocity) || !velocityAllowed(id, latch_velocity)) {
            return rejected_motion(completed);
        }
        auto future{ target.reference(seek_velocity, latch_velocity, timeout) };
        target.m_lastCompletion.then(completed);
        return future;
    }

    pnm::Result<hal::step::PulseCount> Controller::setVelocity(MotorId id,
                                                               pnm::units::AngularVelocity velocity)
    {
        std::future<pnm::Result<hal::step::PulseCount>> result{};
        {
            std::scoped_lock lock{ m_mutex };
            auto& target{ axis(id) };
            if (!canMoveLocked() || !velocityAllowed(id, velocity)) {
                return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
            }
            result = target.requestVelocity(velocity);
        }
        // Admission is serialized; waiting for the motion worker is not.
        return result.get();
    }
    hal::util::Result<> Controller::execute(const Executor::Command& command)
    {
        const auto result{ prepareExecution(command) };
        return result ? startExecution(command.id) : result;
    }
    hal::util::Result<> Controller::prepareExecution(const Executor::Command& command)
    {
        std::scoped_lock lock{ m_mutex };
        return prepareExecutionLocked(command, false);
    }
    hal::util::Result<> Controller::prepareExecutionLocked(const Executor::Command& command, bool owned)
    {
        const auto fail{ [](std::errc e) { return std::unexpected(std::make_error_code(e)); } };
        if ((!owned && !canMoveLocked()) || !m_initialized || m_enabling || !enabled() || !driversReady()) {
            return fail(std::errc::operation_not_permitted);
        }
        const auto selected{ validateExecutionLocked(command) };
        if (!selected) {
            return std::unexpected(selected.error());
        }
        const auto mask{ *selected };
        auto* slot{ std::ranges::find_if(
          m_groups, [](const auto& g) { return !g.managed && (!g.executor || !g.executor->reserved()); }) };
        if (slot == m_groups.end()) {
            return fail(std::errc::device_or_resource_busy);
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (mask & (1U << i)) {
                m_motors[i]->stopAndWait();
            }
        }
        if (!slot->executor) {
            slot->executor = std::make_unique<Executor>(
              m_generator,
              [this](unsigned i, bool forward, std::shared_ptr<const hal::step::Sequence> sequence) {
                return m_motors[i]->prepareCoordinated(forward, std::move(sequence));
            },
              m_profileMemory);
        }
        else if (slot->executor->status().state == Executor::State::Faulted) {
            if (!slot->executor->recover()) {
                return fail(std::errc::state_not_recoverable);
            }
        }
        for (auto& motor : m_motors) {
            motor->m_events->group_notification.store(&m_executionNotification);
        }
        const auto result{ slot->executor->prepare(command) };
        if (result) {
            slot->job = command.id;
            slot->mask = slot->moving = mask;
            slot->notified = slot->started = slot->zero = slot->homing = false;
            slot->error = ::motion::trajectory::Error::None;
            slot->timeout = std::chrono::steady_clock::time_point::max();
            m_lastExecution = command.id;
            m_executionNotification.signal();
        }
        return result;
    }
    hal::util::Result<> Controller::startExecution(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        auto* slot{ std::ranges::find(m_groups, id, &Group::job) };
        if (slot == m_groups.end() || !slot->executor || !m_initialized || m_enabling || !enabled() ||
            !driversReady()) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            if ((slot->mask & (1U << i)) &&
                (!m_motors[i]->isReferenced() || m_motors[i]->coordinatedBlocked())) {
                return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
            }
        }
        const auto result{ slot->executor->start(id) };
        slot->started = bool(result);
        m_executionNotification.signal();
        return result;
    }

}
