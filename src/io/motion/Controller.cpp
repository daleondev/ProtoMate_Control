#include "Controller.hpp"
#include "Limits.hpp"
#include "runtime/thread.hpp"
#include <cmath>

#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#endif

#include <algorithm>
#include <stdexcept>

namespace io::motion
{
    namespace
    {
        using namespace std::chrono_literals;
        constexpr auto DRIVER_POLL_INTERVAL{ 100ms };
        constexpr auto ENABLE_SETTLING_TIME{ 200ms };

        std::future<StepperMotor::Result> rejectedMotion(const std::function<void()>& notify)
        {
            std::promise<StepperMotor::Result> completion{};
            completion.set_value(StepperMotor::Result::Rejected);
            if (notify) {
                notify();
            }
            return completion.get_future();
        }
    }

    Controller::Controller(const Configuration& configuration,
                           const DriverFactory& driver_factory,
                           std::shared_ptr<pnm::log::ISink> log_sink,
                           std::pmr::memory_resource* profile_memory)
      : m_enable{ hal::core::create_steppers_enable_output() }
      , m_generator{ hal::core::create_stepper_generator() }
      , m_configuration{ configuration }
      , m_profileMemory{ profile_memory }
    {
        if (!m_enable || !m_generator) {
            throw std::runtime_error("motion hardware unavailable on this core or already owned");
        }
        m_enable->write(hal::gpio::Level::High);
        // Validate the complete configuration before installing any callbacks.
        for (const auto& config : configuration) {
            if (!config.reference_position.isFinite() || !config.full_step_angle.isFinite() ||
                config.full_step_angle <= 0_deg || config.microsteps == 0) {
                throw std::invalid_argument("invalid motor configuration");
            }
        }
        const std::array microsteps{ configuration[0].microsteps,
                                     configuration[1].microsteps,
                                     configuration[2].microsteps };
        auto drivers{ driver_factory ? driver_factory(microsteps, m_enable)
                                     : hal::core::create_stepper_drivers(microsteps, m_enable) };
        if (std::ranges::any_of(drivers, [](const auto& driver) { return !driver; })) {
            throw std::runtime_error("stepper driver creation failed");
        }
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            const auto id{ static_cast<MotorId>(i) };
            const auto& config{ configuration[i] };
            auto feedback{ hal::core::create_motor_feedback(
              id, { .full_step_angle = config.full_step_angle, .microsteps = config.microsteps }) };
            if (!feedback) {
                throw std::runtime_error("motor feedback creation failed");
            }
            m_motors[i] = std::make_unique<StepperMotor>(id,
                                                         config.reference_position,
                                                         config.full_step_angle,
                                                         config.microsteps,
                                                         m_generator,
                                                         std::move(drivers[i]),
                                                         std::move(feedback),
                                                         [this]() noexcept {
                // May run in an ISR under the driver's lock. Defer all joins,
                // generator access and UART work to the diagnostic worker.
                m_faulted.store(true);
                m_enable->write(hal::gpio::Level::High);
                m_driverNotification.signal();
                m_controlNotification.signal();
                m_executionNotification.signal();
            },
                                                         log_sink);
        }
        m_driverWorker = runtime::thread::create_jthread(
          { .name = "motion diagnostics", .priority = 18, .stack_size = 8192U },
          [this](const std::stop_token& stop) { monitorDrivers(stop); });
    }

    Controller::~Controller()
    {
        // Drop enable before waiting for diagnostics or motion workers.
        m_enable->write(hal::gpio::Level::High);
        m_executionWorker.request_stop();
        m_executionNotification.signal();
        if (m_executionWorker.joinable())
            m_executionWorker.join();
        for (auto& motor : m_motors)
            motor->m_events->group_notification.store(nullptr);
        static_cast<void>(m_driverWorker.request_stop());
        m_driverNotification.signal();
        if (m_driverWorker.joinable()) {
            m_driverWorker.join();
        }
        disable();
        static_cast<void>(m_generator->stop());
        // StepperMotor destruction synchronizes and detaches its callbacks while
        // our notification and shared enable are still alive.
    }

    StepperMotor& Controller::axis(MotorId id) const
    {
        const auto index{ static_cast<std::size_t>(id) };
        if (index >= m_motors.size()) {
            throw std::invalid_argument("invalid motor id");
        }
        return *m_motors[index];
    }

    const StepperMotor& Controller::motor(MotorId id) const { return axis(id); }

    bool Controller::driversReady() const noexcept
    {
        return std::ranges::all_of(m_motors, [](const auto& motor) {
            const auto status{ motor->driver().status() };
            return status.ready && !status.fault_active && !status.fault_latched;
        });
    }

    bool Controller::enabled() const noexcept
    {
        return !m_faulted.load() && m_enable->read() == hal::gpio::Level::Low;
    }

    bool Controller::ready() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_initialized && !m_enabling && !m_faulted.load() && driversReady() &&
               m_generator->status().state == hal::step::State::Running;
    }

    ::motion::trajectory::Reply Controller::trajectorySnapshot() const
    {
        namespace tr = ::motion::trajectory;
        std::scoped_lock lock{ m_mutex };
        tr::Reply result{};
        result.enabled = enabled();
        const auto generator{ m_generator->status() };
        result.counts_exact = generator.counts_exact;
        result.state = result.enabled ? tr::State::Unreferenced : tr::State::Disabled;
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto step{ m_configuration[i].full_step_angle.get() / m_configuration[i].microsteps };
            result.capabilities.step_radians[i] = step;
            result.capabilities.reference_steps[i] = m_configuration[i].reference_position.get() / step;
            // Numerical admission ceilings, not a claim about mechanical limits.
            // A calibrated RobotConfiguration must impose the machine's limits.
            result.capabilities.limits[i] = {
                -tr::MAX_POSITION, tr::MAX_POSITION, limits::MAX_PLANNED_RATE, 1e8, 1e8, 1e10
            };
            const auto count{ tr::quantize(m_motors[i]->setpointPosition().get() / step) };
            if (count) {
                result.position[i] = *count;
            }
            else {
                result.counts_exact = false;
            }
            result.boundary[i].position = result.position[i];
            result.boundary[i].velocity = m_motors[i]->setpointVelocity().get() / step;
            if (m_motors[i]->isReferenced()) {
                result.referenced |= static_cast<std::uint8_t>(1U << i);
            }
        }
        if (result.enabled && result.referenced == 7) {
            result.state = tr::State::Ready;
        }
        if (m_faulted.load() || !result.counts_exact) {
            result.state = tr::State::Faulted;
        }
        if (result.enabled && result.counts_exact && m_executor) {
            switch (m_executor->status().state) {
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
        // The finite profile executor is available through the local Controller
        // API. The richer trajectory/ICC service is not connected to it yet.
        result.capabilities.coordinated_execution = false;
        result.capabilities.hardware_fingerprint = tr::fingerprint(result.capabilities);
        return result;
    }

    bool Controller::canMoveLocked() const
    {
        return (!m_executor || !m_executor->reserved()) && m_initialized && !m_enabling && enabled() &&
               driversReady() && m_generator->status().state == hal::step::State::Running;
    }

    void Controller::initializeDrivers(std::stop_token stop)
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
        if (m_executor && !m_executor->recover())
            throw std::runtime_error("executor recovery failed");
    }

    void Controller::enable(std::stop_token stop)
    {
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
                    permitted = !m_faulted.load() && driversReady();
                    if (permitted) {
                        m_enable->write(hal::gpio::Level::Low);
                    }
                }
                if (!permitted || !enabled() || !driversReady()) {
                    m_faulted.store(true);
                    handleFaultsLocked();
                    throw std::runtime_error("driver verification failed before enable");
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

    void Controller::stopLocked(std::optional<MotorId> id)
    {
        if (m_executor && m_executor->reserved())
            m_executor->abort();
        if (id) {
            axis(*id).stopAndWait();
            return;
        }
        // Abort all outputs before waiting for any individual worker to exit.
        for (auto& motor : m_motors) {
            motor->requestStop();
        }
        for (auto& motor : m_motors) {
            motor->stopAndWait();
        }
    }

    void Controller::stop(std::optional<MotorId> id)
    {
        std::scoped_lock lock{ m_mutex };
        if (id) {
            static_cast<void>(axis(*id)); // Reject invalid IDs before changing state.
        }
        ++m_controlEpoch;
        if (m_enabling) {
            m_enable->write(hal::gpio::Level::High);
        }
        m_controlNotification.signal();
        if (m_executor && m_executor->reserved()) {
            if (id && !(m_executionMask & (1U << static_cast<unsigned>(*id))))
                return;
            m_executor->stop(m_executor->status().id);
            m_executionNotification.signal();
            return;
        }
        stopLocked(id);
    }

    void Controller::invalidateReferencesLocked()
    {
        for (auto& motor : m_motors) {
            motor->invalidateReference();
        }
    }

    void Controller::disable()
    {
        std::scoped_lock lock{ m_mutex };
        ++m_controlEpoch;
        m_controlNotification.signal();
        m_enable->write(hal::gpio::Level::High);
        stopLocked();
        invalidateReferencesLocked();
    }

    void Controller::handleFaultsLocked()
    {
        for (auto& motor : m_motors) {
            const auto status{ motor->driver().status() };
            if (status.fault_active || status.fault_latched || (m_initialized && !status.ready)) {
                m_faulted.store(true);
            }
        }
        if (m_initialized && m_generator->status().state != hal::step::State::Running) {
            m_faulted.store(true);
        }
        if (!m_faulted.load() || m_faultHandled) {
            return;
        }
        m_enable->write(hal::gpio::Level::High);
        stopLocked();
        invalidateReferencesLocked();
        static_cast<void>(m_generator->stop());
        m_initialized = false;
        m_faultHandled = true;
    }

    void Controller::monitorDrivers(const std::stop_token& stop)
    {
        while (!stop.stop_requested()) {
            m_driverNotification.waitUntil(std::chrono::steady_clock::now() + DRIVER_POLL_INTERVAL);
            if (stop.stop_requested()) {
                return;
            }
            std::scoped_lock operation{ m_operationMutex };
            for (auto& motor : m_motors) {
                {
                    std::scoped_lock lock{ m_mutex };
                    handleFaultsLocked();
                    if (!m_initialized) {
                        break;
                    }
                }
                motor->driver().service();
                std::scoped_lock lock{ m_mutex };
                handleFaultsLocked();
            }
        }
    }

    pnm::Result<> Controller::setMotionDefaults(MotorId id, Defaults defaults)
    {
        std::scoped_lock lock{ m_mutex };
        return axis(id).setMotionDefaults(defaults);
    }

    std::future<StepperMotor::Result> Controller::moveRel(MotorId id,
                                                          pnm::units::Angle distance,
                                                          pnm::units::AngularVelocity velocity,
                                                          pnm::units::AngularAcceleration acceleration,
                                                          pnm::units::AngularAcceleration deceleration,
                                                          pnm::units::AngularJerk jerk,
                                                          BufferMode buffer_mode,
                                                          pnm::units::Time timeout,
                                                          std::function<void()> completed)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return rejectedMotion(completed);
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
                                                          std::function<void()> completed)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return rejectedMotion(completed);
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
                                                            std::function<void()> completed)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return rejectedMotion(completed);
        }
        auto future{ target.reference(seek_velocity, latch_velocity, timeout) };
        target.m_lastCompletion.then(completed);
        return future;
    }

    pnm::Result<hal::step::PulseCount> Controller::setVelocity(MotorId id,
                                                               pnm::units::AngularVelocity velocity)
    {
        std::future<pnm::Result<hal::step::PulseCount>> result;
        {
            std::scoped_lock lock{ m_mutex };
            auto& target{ axis(id) };
            if (!canMoveLocked()) {
                return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
            }
            result = target.requestVelocity(velocity);
        }
        // Admission is serialized; waiting for the motion worker is not.
        return result.get();
    }
    hal::util::Result<> Controller::execute(const Executor::Command& command)
    {
        const auto result = prepareExecution(command);
        return result ? startExecution(command.id) : result;
    }
    hal::util::Result<> Controller::prepareExecution(const Executor::Command& command)
    {
        std::scoped_lock lock{ m_mutex };
        if (!canMoveLocked())
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        for (const auto& axis : command.axes)
            if (axis.profile && (!std::isfinite(axis.limits.velocity) ||
                                 axis.limits.velocity > Executor::MAX_PRODUCTION_RATE))
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        for (unsigned i = 0; i < 3; ++i) {
            const auto& motor = *m_motors[i];
            std::scoped_lock queue{ motor.m_queueMutex };
            if (motor.m_busy || motor.m_homing || motor.m_pending)
                return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
            if (command.axes[i].profile &&
                (!motor.isReferenced() || (command.axes[i].forward && motor.referenceSwitchActive())))
                return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        // Completed homing/move threads still own their stacks until joined.
        // Admission above established that all axes are idle. Reclaim those
        // stacks before reserving the execution banks and dedicated worker.
        for (auto& motor : m_motors)
            motor->stopAndWait();
        if (!m_executor) {
            m_executor = std::make_unique<Executor>(
              m_generator,
              [this](unsigned axis, bool forward, std::shared_ptr<const hal::step::Sequence> sequence) {
                return m_motors[axis]->prepareCoordinated(forward, std::move(sequence));
            }, m_profileMemory);
            for (auto& motor : m_motors)
                motor->m_events->group_notification.store(&m_executionNotification);
            try {
                m_executionWorker = runtime::thread::create_jthread(
                  { .name = "motion executor", .priority = 1, .stack_size = 16384U },
                  [this](std::stop_token stop) { executeMotions(stop); });
            } catch (...) {
                for (auto& motor : m_motors)
                    motor->m_events->group_notification.store(nullptr);
                m_executor.reset();
                throw;
            }
        }
        const auto result = m_executor->prepare(command);
        if (result) {
            m_executionMask = 0;
            for (unsigned i = 0; i < 3; ++i)
                if (command.axes[i].profile)
                    m_executionMask |= 1U << i;
            m_executionNotification.signal();
        }
        return result;
    }
    hal::util::Result<> Controller::startExecution(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_executor || !m_initialized || m_enabling || !enabled() || !driversReady())
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        for (unsigned i = 0; i < 3; ++i)
            if ((m_executionMask & (1U << i)) &&
                (!m_motors[i]->isReferenced() || m_motors[i]->coordinatedBlocked()))
                return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        const auto result = m_executor->start(id);
        m_executionNotification.signal();
        return result;
    }
    bool Controller::renewExecution(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        return m_executor && m_executor->renew(id);
    }
    bool Controller::stopExecution(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        const bool accepted = m_executor && m_executor->stop(id);
        m_executionNotification.signal();
        return accepted;
    }
    Executor::Status Controller::executionStatus() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_executor ? m_executor->status() : Executor::Status{};
    }
    void Controller::executeMotions(std::stop_token stop)
    {
        const std::stop_callback wake{ stop, [this] { m_executionNotification.signal(); } };
        while (!stop.stop_requested()) {
            bool active{};
            {
                std::scoped_lock lock{ m_mutex };
                active = m_executor && m_executor->active();
                if (active) {
                    bool blocked = m_faulted.load();
                    for (unsigned i = 0; i < 3; ++i)
                        blocked |= (m_executionMask & (1U << i)) &&
                                   (!m_motors[i]->isReferenced() || m_motors[i]->coordinatedBlocked());
                    if (blocked) {
                        m_executor->abort();
                        m_enable->write(hal::gpio::Level::High);
                        invalidateReferencesLocked();
                    }
                    else
                        m_executor->service();
                    if (m_executor->status().state == Executor::State::Faulted) {
                        m_faulted.store(true);
                        m_enable->write(hal::gpio::Level::High);
                        m_driverNotification.signal();
                    }
                    active = m_executor->active();
                }
            }
            m_executionNotification.waitUntil(active ? std::chrono::steady_clock::now() + 1ms
                                                     : std::chrono::steady_clock::time_point::max());
        }
        std::scoped_lock lock{ m_mutex };
        if (m_executor && m_executor->reserved())
            m_executor->abort();
    }

}
