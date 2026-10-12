#include "Server.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace io::motion
{
    namespace
    {
        using namespace std::chrono_literals;
        using namespace pnm::units;
        using namespace ::motion;

        Controller::MotorId id(MotorId motor)
        {
            if (motor >= MotorId::All) { throw std::invalid_argument("motor must be m1, m2 or m3"); }
            return static_cast<Controller::MotorId>(motor);
        }

        bool valid(const MotorGoal& goal)
        {
            return goal.motor < MotorId::All && goal.motion <= MotorMotion::Home &&
                   goal.buffer <= MotorBuffer::BlendingHigh &&
                   std::isfinite(goal.position) && std::isfinite(goal.velocity) && goal.velocity > 0 &&
                   std::isfinite(goal.acceleration) && goal.acceleration >= 0 &&
                   std::isfinite(goal.deceleration) && goal.deceleration >= 0 &&
                   std::isfinite(goal.jerk) && goal.jerk >= 0 &&
                   std::isfinite(goal.timeout) && goal.timeout >= 0 && goal.timeout <= 1e8 &&
                   (goal.motion != MotorMotion::Home ||
                    (goal.timeout > 0 && std::isfinite(goal.latch_velocity) &&
                     goal.latch_velocity > 0 && goal.latch_velocity < goal.velocity));
        }
    }

    Server::Server(pnm::msg::Bus& bus, Controller& controller)
      : m_controller{ &controller }
      , m_commands{ bus.service<^^icc::Services::io_motor>().serve(
          [this](const MotorRequest& value) { return request(value, false); }, { .max_pending = 4 }) }
      , m_urgent{ bus.service<^^icc::Services::io_motor_stop>().serve(
          [this](const MotorRequest& value) { return request(value, true); }, { .max_pending = 4 }) }
      , m_motions{ bus.action<^^icc::Actions::io_motor_motion>().serveDeferred(
          [this](const MotorGoal& goal, Execution execution) { admit(goal, std::move(execution)); },
          { .max_goals = 8 }) }
    {
    }

    MotorProgress Server::progress(MotorId motor) const
    {
        const auto& axis{ m_controller->motor(id(motor)) };
        return { axis.setpointPosition().get<AngleUnits::deg>(),
                 axis.setpointVelocity().get<AngularVelocityUnits::rpm>(), axis.isReferenced() };
    }

    MotorResponse Server::snapshot() const
    {
        MotorResponse result;
        result.generation = m_generation;
        result.enabled = m_controller->enabled();
        result.ready = m_controller->ready();
        for (std::size_t i{}; i < result.motors.size(); ++i) {
            const auto& motor{ m_controller->motor(static_cast<Controller::MotorId>(i)) };
            auto& state{ result.motors[i] };
            const auto position{ motor.actualPosition() };
            const auto velocity{ motor.actualVelocity() };
            const auto defaults{ motor.motionDefaults() };
            const auto driver{ motor.driver().status() };
            state.position = motor.setpointPosition().get<AngleUnits::deg>();
            state.velocity = motor.setpointVelocity().get<AngularVelocityUnits::rpm>();
            state.position_valid = position.has_value();
            state.velocity_valid = velocity.has_value();
            if (position) { state.actual_position = position->get<AngleUnits::deg>(); }
            if (velocity) { state.actual_velocity = velocity->get<AngularVelocityUnits::rpm>(); }
            state.referenced = motor.isReferenced();
            state.switch_active = motor.referenceSwitchActive();
            state.feedback = motor.feedbackSource() == StepperMotor::FeedbackSource::DriverIndex
                               ? MotorFeedback::Index : MotorFeedback::Encoder;
            state.feedback_resolution = motor.feedbackResolution().get<AngleUnits::deg>();
            state.acceleration = defaults.acceleration.get<AngularAccelerationUnits::deg_s2>();
            state.deceleration = defaults.deceleration.get<AngularAccelerationUnits::deg_s2>();
            state.jerk = defaults.jerk.get<AngularJerkUnits::deg_s3>();
            state.driver = motor.driver().name().substr(0, 48);
            state.driver_ready = driver.ready;
            state.fault_active = driver.fault_active;
            state.fault_latched = driver.fault_latched;
            state.warning = driver.warning;
            state.fault = static_cast<std::uint8_t>(driver.fault);
        }
        return result;
    }

    MotorResponse Server::request(const MotorRequest& value, bool urgent)
    {
        MotorResponse result;
        try {
            if (m_stop.stop_requested()) { throw std::runtime_error("IO controller is stopping"); }
            if (value.motor > MotorId::All) { throw std::invalid_argument("invalid motor"); }
            const bool stopping{ value.operation == MotorOperation::Stop || value.operation == MotorOperation::Disable };
            if (urgent != stopping) { throw std::invalid_argument("incorrect motor command lane"); }
            switch (value.operation) {
                case MotorOperation::Status: break;
                case MotorOperation::Initialize: ++m_generation; m_controller->initializeDrivers(); break;
                case MotorOperation::Enable: m_controller->enable(); break;
                case MotorOperation::Disable: ++m_generation; m_controller->disable(); break;
                case MotorOperation::Stop:
                    ++m_generation;
                    m_controller->stop(value.motor == MotorId::All ? std::nullopt : std::optional{ id(value.motor) });
                    break;
                case MotorOperation::Defaults: {
                    auto defaults{ m_controller->motor(id(value.motor)).motionDefaults() };
                    if (value.dynamics_fields > 7) { throw std::invalid_argument("invalid dynamics fields"); }
                    if (value.dynamics_fields & 1) { defaults.acceleration = value.acceleration * 1_deg_s2; }
                    if (value.dynamics_fields & 2) { defaults.deceleration = value.deceleration * 1_deg_s2; }
                    if (value.dynamics_fields & 4) { defaults.jerk = value.jerk * 1_deg_s3; }
                    if (value.dynamics_fields) {
                        const auto changed{ m_controller->setMotionDefaults(id(value.motor), defaults) };
                        if (!changed) { throw std::runtime_error(changed.error().message()); }
                    }
                    break;
                }
                case MotorOperation::Speed: {
                    const auto changed{ m_controller->setVelocity(id(value.motor), value.velocity * 1_rpm) };
                    if (!changed) { throw std::runtime_error(changed.error().message()); }
                    result.pulse = *changed;
                    break;
                }
                default: throw std::invalid_argument("unknown motor operation");
            }
            const auto pulse{ result.pulse };
            result = snapshot();
            result.pulse = pulse;
        } catch (const std::exception& error) {
            result.ok = false;
            result.message = std::string_view{ error.what() }.substr(0, 160);
        }
        return result;
    }

    void Server::admit(const MotorGoal& goal, Execution execution)
    {
        if (m_stop.stop_requested() || !valid(goal) || goal.generation != m_generation ||
            !m_controller->ready() || !m_controller->enabled() ||
            (goal.motion == MotorMotion::Absolute && !m_controller->motor(id(goal.motor)).isReferenced())) {
            execution.reject();
            return;
        }
        if (execution.cancelRequested()) { execution.cancelled({ MotorOutcome::Stopped, {} }); return; }
        auto slot{ std::ranges::find_if(m_jobs, [](const auto& job) { return !job; }) };
        if (slot == m_jobs.end()) { execution.fail(pnm::msg::ActionError::Busy); return; }
        auto finished{ std::make_shared<Finished>() };
        auto cancellation{ pnm::msg::watch(execution, [notification = m_notification] { notification->signal(); }) };
        std::function<void()> completed{ [finished, notification = m_notification] {
            finished->ready.store(true);
            notification->signal();
        } };
        if (!execution.accept()) { return; }
        slot->emplace(Job{ goal.motor, std::move(execution), finished, {}, std::move(cancellation) });
        try {
            const auto motor{ id(goal.motor) };
            if (goal.motion == MotorMotion::Home) {
                (*slot)->result = m_controller->reference(motor, goal.velocity * 1_rpm,
                  goal.latch_velocity * 1_rpm, goal.timeout * 1_s, std::move(completed));
            } else {
                const auto move = goal.motion == MotorMotion::Relative ? &Controller::moveRel : &Controller::moveAbs;
                (*slot)->result = (m_controller->*move)(motor, goal.position * 1_deg, goal.velocity * 1_rpm,
                  goal.acceleration * 1_deg_s2, goal.deceleration * 1_deg_s2, goal.jerk * 1_deg_s3,
                  static_cast<BufferMode>(goal.buffer), goal.timeout * 1_s, std::move(completed));
            }
        } catch (...) {
            m_controller->stop(id(goal.motor));
            (*slot)->execution.abort({ MotorOutcome::Faulted, progress(goal.motor) });
            slot->reset();
        }
    }

    void Server::dispatchJobs(bool feedback)
    {
        for (auto& job : m_jobs) {
            if (!job) { continue; }
            if (!job->finished->ready.load() && job->execution.cancelRequested()) {
                ++m_generation;
                // Cancellation is an axis stop, including its buffered successor.
                // Join before reporting a terminal result; other motors keep running.
                m_controller->stop(id(job->motor));
            }
            if (job->finished->ready.load()) {
                auto outcome{ StepperMotor::Result::Faulted };
                try { outcome = job->result.get(); } catch (...) {}
                const MotorResult result{ static_cast<MotorOutcome>(outcome), progress(job->motor) };
                if (outcome == StepperMotor::Result::Completed) { job->execution.succeed(result); }
                else if (outcome == StepperMotor::Result::Stopped) { job->execution.cancelled(result); }
                else { job->execution.abort(result); }
                job.reset();
            } else if (feedback) { job->execution.feedback(progress(job->motor)); }
        }
    }

    void Server::run(std::stop_token stop)
    {
        m_stop = stop;
        const auto wake{ [notification = m_notification] { notification->signal(); } };
        const auto commands{ pnm::msg::watch(m_commands, wake) };
        const auto urgent{ pnm::msg::watch(m_urgent, wake) };
        const auto actions{ pnm::msg::watch(m_motions, wake) };
        const std::stop_callback cancellation{ stop, wake };
        auto feedback{ std::chrono::steady_clock::now() };
        try {
            while (!stop.stop_requested()) {
                m_urgent.poll();
                dispatchJobs(false);
                m_commands.poll();
                m_motions.poll();
                const auto now{ std::chrono::steady_clock::now() };
                dispatchJobs(now >= feedback);
                if (now >= feedback) { feedback = now + 20ms; }
                const bool active{ std::ranges::any_of(m_jobs, [](const auto& job) { return job.has_value(); }) };
                m_notification->waitUntil(active ? feedback : std::chrono::steady_clock::time_point::max());
            }
        } catch (...) {
            m_controller->disable();
            m_motions.close();
            m_commands.close();
            m_urgent.close();
            throw;
        }
        m_commands.close();
        m_urgent.close();
        m_motions.requestStop();
        m_controller->disable();
        dispatchJobs(false);
        m_motions.close();
    }
}
