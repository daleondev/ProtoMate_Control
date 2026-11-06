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


    }

    Controller::Controller(const Configuration& configuration,
                           const DriverFactory& driver_factory,
                           std::shared_ptr<pnm::log::ISink> log_sink,
                           std::pmr::memory_resource* profile_memory, const Hardware* hardware)
      : m_enable{ hardware && hardware->enable ? hardware->enable : hal::core::create_steppers_enable_output() }
      , m_generator{ hardware && hardware->generator ? hardware->generator : hal::core::create_stepper_generator() }
      , m_configuration{ configuration }
      , m_profileMemory{ profile_memory }
    {
        if (!m_enable || !m_generator) {
            throw std::runtime_error("motion hardware unavailable on this core or already owned");
        }
        m_enable->write(hal::gpio::Level::High);
        m_boot = std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1U;
        // Validate the complete configuration before installing any callbacks.
        for (const auto& config : configuration) {
            if (!config.reference_position.isFinite() || !config.full_step_angle.isFinite() ||
                config.full_step_angle <= 0_deg || config.microsteps == 0) {
                throw std::invalid_argument("invalid motor configuration");
            }
        }
        initializeCapabilities();
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
                                                         log_sink, hardware ? hardware->references[i] : nullptr);
        }
        m_driverWorker = runtime::thread::create_jthread(
          // Physical TMC diagnostics peaked below 3.5 KiB; retain over 40%
          // headroom on ARM rather than budgeting from the synthetic driver.
          { .name = "motion diagnostics", .priority = 18,
            .stack_size = sizeof(void*) == 4 ? 6144U : 8192U },
          [this](const std::stop_token& stop) { monitorDrivers(stop); });
        if (!m_generator->setRefillNotification([this] { m_executionNotification.signal(); }))
            throw std::runtime_error("motion refill worker unavailable");
        try {
            m_executionWorker = runtime::thread::create_jthread(
              { .name = "motion executor", .priority = 1, .stack_size = 16384U },
              [this](std::stop_token stop) { executeMotions(stop); });
        } catch (...) {
            static_cast<void>(m_generator->setRefillNotification({}));
            throw;
        }
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
        static_cast<void>(m_generator->setRefillNotification({}));
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
        return !m_admissionLost && m_initialized && !m_enabling && !m_faulted.load() && driversReady() &&
               m_generator->status().state == hal::step::State::Running;
    }




    bool Controller::canMoveLocked() const
    {
        return (m_ownership.family() == ::motion::Family::None ||
                m_ownership.family() == ::motion::Family::Motor) &&
               std::ranges::none_of(m_groups, [](const auto& g) { return g.executor && g.executor->reserved(); }) &&
               !m_admissionLost && m_initialized && !m_enabling && enabled() &&
               driversReady() && m_generator->status().state == hal::step::State::Running;
    }
    bool Controller::velocityAllowed(MotorId id, pnm::units::AngularVelocity velocity) const
    {
        const auto& config = m_configuration.at(static_cast<unsigned>(id));
        return velocity.isFinite() && std::abs(velocity.get()) <=
          Executor::MAX_PRODUCTION_RATE * config.full_step_angle.get() / config.microsteps;
    }





    void Controller::stopLocked(std::optional<MotorId> id)
    {
        stopProgramsLocked(id ? std::uint8_t(1U << static_cast<unsigned>(*id)) : 7U, true);
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
        const auto mask = id ? std::uint8_t(1U << static_cast<unsigned>(*id)) : 7U;
        bool group{};
        for (const auto& g : m_groups)
            group |= (g.mask & mask) && g.executor && g.executor->reserved();
        if (group) {
            stopProgramsLocked(mask, false);
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

    void Controller::abort(std::optional<MotorId> id)
    {
        std::scoped_lock lock{m_mutex};
        if (id) static_cast<void>(axis(*id));
        auto mask=id?std::uint8_t(1U<<static_cast<unsigned>(*id)):std::uint8_t(7);
        for(const auto& g:m_groups)
            if((g.managed || (g.executor && g.executor->reserved())) && (g.mask&mask)) mask|=g.mask;
        stopProgramsLocked(mask,true);
        for(unsigned i=0;i<3;++i)if(mask&(1U<<i)){
            m_motors[i]->requestStop();m_motors[i]->invalidateReference();
        }
        ++m_controlEpoch;m_ownership.changed(mask);
        m_executionNotification.signal();
    }

    void Controller::disable()
    {
        std::scoped_lock lock{ m_mutex };
        ++m_controlEpoch;
        m_controlNotification.signal();
        m_enable->write(hal::gpio::Level::High);
        m_ownership.invalidate();
        stopLocked();
        invalidateReferencesLocked();
        finishProgramsLocked();
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
        m_ownership.invalidate();
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

    bool Controller::renewExecution(std::uint64_t id)
    {
        std::scoped_lock lock{m_mutex};
        const auto slot = std::ranges::find(m_groups, id, &Group::job);
        return slot != m_groups.end() && slot->executor && slot->executor->renew(id);
    }
    bool Controller::stopExecution(std::uint64_t id)
    {
        std::scoped_lock lock{m_mutex};
        const auto slot = std::ranges::find(m_groups, id, &Group::job);
        const bool accepted = slot != m_groups.end() && slot->executor && slot->executor->stop(id);
        m_executionNotification.signal();
        return accepted;
    }
    Executor::Status Controller::executionStatus(std::uint64_t id) const
    {
        std::scoped_lock lock{m_mutex};
        const auto slot = std::ranges::find(m_groups, id ? id : m_lastExecution, &Group::job);
        return slot != m_groups.end() && slot->executor ? slot->executor->status() : Executor::Status{};
    }
    void Controller::executeMotions(std::stop_token stop)
    {
        const std::stop_callback wake{stop, [this] { m_executionNotification.signal(); }};
        while (!stop.stop_requested()) {
            m_generator->preparePending();
            bool active{};
            {
                std::scoped_lock lock{m_mutex};
                const auto now = std::chrono::steady_clock::now();
                for (auto& g : m_groups) {
                    if (g.managed && g.homing && g.started && !g.home_finished) {
                        serviceHomeLocked(g);active=true;continue;
                    }
                    if (!g.executor || !g.executor->active()) continue;
                    bool blocked = m_faulted.load();
                    for (unsigned i=0; i<3; ++i)
                        blocked |= (g.mask & (1U<<i)) &&
                                   (!m_motors[i]->isReferenced() || ((g.moving & (1U<<i)) && m_motors[i]->coordinatedBlocked()));
                    if (blocked) {
                        g.executor->abort();
                        for (unsigned i=0; i<3; ++i)
                            if (g.mask & (1U<<i)) m_motors[i]->invalidateReference();
                    } else {
                        if (g.managed) {
                            const auto* owner = m_ownership.find(g.job);
                            if (!owner || now >= owner->deadline || owner->state == ::motion::JobState::Stopping ||
                                now >= g.timeout) {
                                g.executor->stop(g.executor->status().id);
                                if (owner && now >= owner->deadline) g.error = ::motion::trajectory::Error::LeaseExpired;
                            } else g.executor->renew(g.executor->status().id);
                        }
                        g.executor->service();
                    }
                    if (g.executor->status().state == Executor::State::Faulted) {
                        m_faulted.store(true);
                        m_enable->write(hal::gpio::Level::High);
                        m_driverNotification.signal();
                    }
                    active |= g.executor->active();
                }
                finishProgramsLocked();
                // Planning reservations and homing/raw jobs also have leases.
                for (const auto& entry : m_ownership.entries()) {
                    if (!entry.job) continue;
                    active = true;
                    if (now < entry.deadline || entry.state >= ::motion::JobState::Stopping) continue;
                    auto* owner = m_ownership.find(entry.job);
                    owner->state = ::motion::JobState::Stopping;
                    bool group{};
                    for (auto& g : m_groups) if (g.managed && g.job == entry.job) {
                        group = true;
                        g.error = ::motion::trajectory::Error::LeaseExpired;
                        if (g.executor) g.executor->stop(g.executor->status().id);
                        if (!g.started) {
                            g.managed=false;g.homing=false;g.completed={};
                            m_ownership.release(entry.job);
                        }
                    }
                    if (!group) {
                        for (unsigned i=0; i<3; ++i) if (entry.axes & (1U<<i)) m_motors[i]->requestStop();
                        if (entry.family != ::motion::Family::Motor) m_ownership.release(entry.job);
                    }
                }
            }
            m_executionNotification.waitUntil(active ? std::chrono::steady_clock::now() + 1ms
                                                     : std::chrono::steady_clock::time_point::max());
        }
        std::scoped_lock lock{m_mutex};
        stopProgramsLocked(7, true);
    }
}
