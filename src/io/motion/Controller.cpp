#include "Controller.hpp"
#include "runtime/thread.hpp"

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

        std::future<StepperMotor::Result> rejectedMotion()
        {
            std::promise<StepperMotor::Result> completion;
            completion.set_value(StepperMotor::Result::Rejected);
            return completion.get_future();
        }
    }

    Controller::Controller(const Configuration& configuration, const DriverFactory& driver_factory,
                           std::shared_ptr<pnm::log::ISink> log_sink)
      : m_enable{ hal::core::create_steppers_enable_output() }
      , m_generator{ hal::core::create_stepper_generator() }
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
            m_motors[i] = std::make_unique<StepperMotor>(
              id, config.reference_position, config.full_step_angle, config.microsteps,
              m_generator, std::move(drivers[i]), std::move(feedback), [this]() noexcept {
                  // May run in an ISR under the driver's lock. Defer all joins,
                  // generator access and UART work to the diagnostic worker.
                  m_faulted.store(true);
                  m_enable->write(hal::gpio::Level::High);
                  m_driverNotification.signal();
              }, log_sink);
        }
        m_driverWorker = runtime::thread::create_jthread(
          { .name = "motion diagnostics", .priority = 18, .stack_size = 8192U },
          [this](const std::stop_token& stop) { monitorDrivers(stop); });
    }

    Controller::~Controller()
    {
        // Drop enable before waiting for diagnostics or motion workers.
        m_enable->write(hal::gpio::Level::High);
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
        return m_initialized && !m_faulted.load() && driversReady() &&
               m_generator->status().state == hal::step::State::Running;
    }

    bool Controller::canMoveLocked() const
    {
        return m_initialized && enabled() && driversReady() &&
               m_generator->status().state == hal::step::State::Running;
    }

    void Controller::initializeDrivers()
    {
        std::scoped_lock lock{ m_mutex };
        if (m_enable->read() != hal::gpio::Level::High) {
            throw std::logic_error("disable motors before initializing drivers");
        }
        m_initialized = false;
        stopLocked();
        invalidateReferencesLocked();
        static_cast<void>(m_generator->stop());
        m_faulted.store(false);
        m_faultHandled = false;
        bool failed{};
        for (auto& motor : m_motors) {
            failed |= !motor->driver().initialize();
        }
        handleFaultsLocked();
        if (failed || m_faulted.load() || !driversReady()) {
            throw std::runtime_error("driver initialization failed; inspect motor driver status");
        }
        if (const auto result{ m_generator->start() }; !result) {
            throw std::runtime_error("STEP timebase start failed: " + result.error().message());
        }
        m_initialized = true;
    }

    void Controller::enable()
    {
        std::scoped_lock lock{ m_mutex };
        handleFaultsLocked();
        if (!m_initialized || m_faulted.load() || !driversReady()) {
            throw std::runtime_error("initialize all drivers before enabling motors");
        }
        if (enabled()) {
            return;
        }
        for (auto& motor : m_motors) {
            if (!motor->driver().verify()) {
                m_faulted.store(true);
            }
        }
        handleFaultsLocked();
        bool permitted{};
        {
#ifdef HAL_PLATFORM_STM32
            // Do not overwrite an alarm ISR's disable write with our enable.
            const hal::stm32::InterruptGuard guard;
#endif
            permitted = !m_faulted.load() && driversReady();
            if (permitted) {
                m_enable->write(hal::gpio::Level::Low);
            }
        }
        // Also closes the concurrent callback window in host tests.
        if (!permitted || m_faulted.load() || !driversReady()) {
            m_enable->write(hal::gpio::Level::High);
            m_faulted.store(true);
            handleFaultsLocked();
            throw std::runtime_error("driver verification failed before enable");
        }
        std::this_thread::sleep_for(ENABLE_SETTLING_TIME);
        if (!enabled() || !driversReady()) {
            m_enable->write(hal::gpio::Level::High);
            m_faulted.store(true);
            handleFaultsLocked();
            throw std::runtime_error("driver fault during enable settling");
        }
    }

    void Controller::stopLocked(std::optional<MotorId> id)
    {
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
            std::scoped_lock lock{ m_mutex };
            handleFaultsLocked();
            for (auto& motor : m_motors) {
                // No UART polling until explicitly initialized. A fault requires
                // explicit recovery rather than automatically restoring readiness.
                if (!m_initialized) {
                    break;
                }
                motor->driver().service();
                handleFaultsLocked();
            }
        }
    }

    pnm::Result<> Controller::setMotionDefaults(MotorId id, Defaults defaults)
    {
        std::scoped_lock lock{ m_mutex };
        return axis(id).setMotionDefaults(defaults);
    }

    std::future<StepperMotor::Result> Controller::moveRel(
      MotorId id, pnm::units::Angle distance, pnm::units::AngularVelocity velocity,
      pnm::units::AngularAcceleration acceleration, pnm::units::AngularAcceleration deceleration,
      pnm::units::AngularJerk jerk, BufferMode buffer_mode, pnm::units::Time timeout)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return rejectedMotion();
        }
        return target.moveRel(distance, velocity, acceleration, deceleration, jerk, buffer_mode, timeout);
    }

    std::future<StepperMotor::Result> Controller::moveAbs(
      MotorId id, pnm::units::Angle position, pnm::units::AngularVelocity velocity,
      pnm::units::AngularAcceleration acceleration, pnm::units::AngularAcceleration deceleration,
      pnm::units::AngularJerk jerk, BufferMode buffer_mode, pnm::units::Time timeout)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return rejectedMotion();
        }
        return target.moveAbs(position, velocity, acceleration, deceleration, jerk, buffer_mode, timeout);
    }

    std::future<StepperMotor::Result> Controller::reference(
      MotorId id, pnm::units::AngularVelocity seek_velocity,
      pnm::units::AngularVelocity latch_velocity, pnm::units::Time timeout)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return rejectedMotion();
        }
        return target.reference(seek_velocity, latch_velocity, timeout);
    }

    pnm::Result<hal::step::PulseCount> Controller::setVelocity(
      MotorId id, pnm::units::AngularVelocity velocity)
    {
        std::scoped_lock lock{ m_mutex };
        auto& target{ axis(id) };
        if (!canMoveLocked()) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        return target.setVelocity(velocity);
    }
}
