#include "MotionController.hpp"

#include <algorithm>
#include <stdexcept>

namespace control
{
    using namespace std::chrono_literals;

    MotionController::MotionController(const std::array<AxisConfig, 3>& configuration)
      : m_enable{ hal::board::createSteppersEnableOutput() }
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
        }
        if (const auto result{ m_generator->start() }; !result)
            throw std::runtime_error("step timebase start failed: " + result.error().message());
    }

    MotionController::~MotionController()
    {
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
        if (m_enable->read() == hal::gpio::Level::Low)
            return;
        m_enable->write(hal::gpio::Level::Low);
        std::this_thread::sleep_for(200ms);
    }

    void MotionController::stopLocked(std::optional<MotorId> motor)
    {
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
        if (const auto result{ axis(motor).setMotionDefaults(defaults) }; !result)
            throw std::runtime_error("cannot set motion defaults: " + result.error().message());
    }

    MotionController::Status MotionController::status()
    {
        std::scoped_lock lock{ m_mutex };
        const auto generator{ m_generator->status() };
        collect();
        Status status{ .enabled = m_enable->read() == hal::gpio::Level::Low,
                       .generator = generator,
                       .axes = {} };
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
                               static_cast<std::size_t>(
                                 std::ranges::count_if(m_motions, [id](const auto& entry) {
                return entry.motor == id && !entry.result;
            })) };
        }
        return status;
    }

    std::vector<MotionController::Motion> MotionController::motions()
    {
        std::scoped_lock lock{ m_mutex };
        collect();
        return { m_motions.begin(), m_motions.end() };
    }
}
