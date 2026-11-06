#include "Server.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <pneumo/logging.hpp>

namespace io::motion
{
    namespace
    {
        using namespace std::chrono_literals;
        using namespace pnm::units;
        using namespace ::motion;

        Controller::MotorId id(MotorId motor)
        {
            if (motor >= MotorId::All) {
                throw std::invalid_argument("motor must be m1, m2 or m3");
            }
            return static_cast<Controller::MotorId>(motor);
        }

        bool valid(const MotorGoal& goal)
        {
            return goal.motor < MotorId::All && goal.motion <= MotorMotion::Home &&
                   goal.buffer <= MotorBuffer::BlendingHigh && std::isfinite(goal.position) &&
                   std::isfinite(goal.velocity) && goal.velocity > 0 && std::isfinite(goal.acceleration) &&
                   goal.acceleration >= 0 && std::isfinite(goal.deceleration) && goal.deceleration >= 0 &&
                   std::isfinite(goal.jerk) && goal.jerk >= 0 && std::isfinite(goal.timeout) &&
                   goal.timeout >= 0 && goal.timeout <= 1e8 &&
                   (goal.motion != MotorMotion::Home ||
                    (goal.timeout > 0 && std::isfinite(goal.latch_velocity) && goal.latch_velocity > 0 &&
                     goal.latch_velocity < goal.velocity));
        }
    }

    Server::Server(pnm::msg::Bus& bus, Controller& controller)
      : m_controller{ &controller }
      , m_state{ bus.topic<^^icc::Topics::io_motor_state>() }
      , m_commands{ bus.service<^^icc::Services::io_motor>().serveDeferred(
          [this](const MotorRequest& value, Reply reply) { deferCommand(value, std::move(reply)); },
          { .max_pending = 4 }) }
      , m_urgent{ bus.service<^^icc::Services::io_motor_stop>().serve(
          [this](const MotorRequest& value) {
              auto result{ request(value, true, m_stop) };
              result.commandGeneration = m_commandGeneration;
              return result;
          },
          { .max_pending = 4 }) }
      , m_motions{ bus.action<^^icc::Actions::io_motor_motion>().serveDeferred(
          [this](const MotorGoal& goal, Execution execution) { admit(goal, std::move(execution)); },
          { .max_goals = 8 }) }
      , m_trajectories{ bus.service<^^icc::Services::io_trajectory>().serveDeferred(
          [this](const ::motion::trajectory::Packet& packet, ProgramReply reply) {
              deferProgram(packet, std::move(reply));
          }, { .max_pending = 2 }) }
      , m_trajectoryControl{bus.service<^^icc::Services::io_trajectory_control>().serve(
          [this](const ::motion::trajectory::Packet& packet) { return trajectory(packet); },
          {.max_pending=4})}
      , m_programs{bus.action<^^icc::Actions::io_motion>().serveDeferred(
          [this](const ::motion::trajectory::Run& run, ProgramExecution execution) {
              admitProgram(run, std::move(execution));
          }, {.max_goals=3})}
    {
    }

    ::motion::trajectory::Packet Server::trajectory(const ::motion::trajectory::Packet& packet)
    {
        namespace tr = ::motion::trajectory;
        const auto decoded = tr::decodeRequest(packet);
        if (!decoded) return tr::encode(tr::Reply{.error=decoded.error()});
        const auto op = decoded->operation;
        if (op != tr::Operation::Stop && op != tr::Operation::Abort &&
            op != tr::Operation::Renew && op != tr::Operation::Release)
            return tr::encode(tr::Reply{.error=tr::Error::Protocol});
        auto result = m_controller->control(*decoded);
        if(result.error==tr::Error::None && (op==tr::Operation::Stop || op==tr::Operation::Abort)) {
            ++m_commandGeneration;
            discardQueuedCommands();
        }
        result.generation = m_commandGeneration;
        return tr::encode(result);
    }

    MotorProgress Server::progress(MotorId motor) const
    {
        const auto& axis{ m_controller->motor(id(motor)) };
        return { axis.setpointPosition().get<AngleUnits::deg>(),
                 axis.setpointVelocity().get<AngularVelocityUnits::rpm>(),
                 axis.isReferenced() };
    }

    MotorSnapshot Server::snapshot()
    {
        MotorSnapshot result{};
        result.commandGeneration = m_commandGeneration;
        result.sequence = ++m_sequence;
        result.sampled_at_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
        result.enabled = m_controller->enabled();
        result.ready = m_controller->ready();
        const auto ownership = m_controller->trajectorySnapshot();
        result.boot = ownership.boot;
        result.family = ownership.family; result.owners = ownership.owners;
        result.revisions = ownership.axis_revisions;
        result.configuration = ownership.capabilities.hardware_fingerprint;
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
            if (position) {
                state.actual_position = position->get<AngleUnits::deg>();
            }
            if (velocity) {
                state.actual_velocity = velocity->get<AngularVelocityUnits::rpm>();
            }
            state.referenced = motor.isReferenced();
            state.switch_active = motor.referenceSwitchActive();
            state.feedback = motor.feedbackSource() == StepperMotor::FeedbackSource::DriverIndex
                               ? MotorFeedback::Index
                               : MotorFeedback::Encoder;
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

    MotorResponse Server::request(const MotorRequest& value, bool urgent, std::stop_token stop)
    {
        MotorResponse result{};
        try {
            if (stop.stop_requested()) {
                throw std::runtime_error("motor command cancelled");
            }
            if (value.motor > MotorId::All) {
                throw std::invalid_argument("invalid motor");
            }
            const bool stopping{ value.operation == MotorOperation::Stop ||
                                 value.operation == MotorOperation::Disable };
            if (urgent != stopping) {
                throw std::invalid_argument("incorrect motor command lane");
            }
            switch (value.operation) {
                case MotorOperation::Initialize:
                    m_controller->initializeDrivers(stop);
                    break;
                case MotorOperation::Enable:
                    m_controller->enable(stop);
                    break;
                case MotorOperation::Disable:
                    ++m_commandGeneration;
                    cancelCommand();
                    m_controller->disable();
                    discardQueuedCommands();
                    break;
                case MotorOperation::Stop:
                    ++m_commandGeneration;
                    cancelCommand();
                    m_controller->abort(value.motor == MotorId::All ? std::nullopt
                                                                   : std::optional{ id(value.motor) });
                    discardQueuedCommands();
                    break;
                case MotorOperation::Defaults: {
                    auto defaults{ m_controller->motor(id(value.motor)).motionDefaults() };
                    if (value.dynamics_fields > 7) {
                        throw std::invalid_argument("invalid dynamics fields");
                    }
                    if (value.dynamics_fields & 1) {
                        defaults.acceleration = value.acceleration * 1_deg_s2;
                    }
                    if (value.dynamics_fields & 2) {
                        defaults.deceleration = value.deceleration * 1_deg_s2;
                    }
                    if (value.dynamics_fields & 4) {
                        defaults.jerk = value.jerk * 1_deg_s3;
                    }
                    if (value.dynamics_fields) {
                        const auto changed{ m_controller->setMotionDefaults(id(value.motor), defaults) };
                        if (!changed) {
                            throw std::runtime_error(changed.error().message());
                        }
                    }
                    result.defaults = { defaults.acceleration.get<AngularAccelerationUnits::deg_s2>(),
                                        defaults.deceleration.get<AngularAccelerationUnits::deg_s2>(),
                                        defaults.jerk.get<AngularJerkUnits::deg_s3>() };
                    break;
                }
                case MotorOperation::Speed: {
                    const auto changed{ m_controller->setVelocity(id(value.motor), value.velocity * 1_rpm) };
                    if (!changed) {
                        throw std::runtime_error(changed.error().message());
                    }
                    result.pulse = *changed;
                    break;
                }
                default:
                    throw std::invalid_argument("unknown motor operation");
            }
        } catch (const std::exception& error) {
            result.ok = false;
            result.message = std::string_view{ error.what() }.substr(0, 160);
        }
        return result;
    }

    void Server::deferCommand(const MotorRequest& value, Reply reply)
    {
        finishCommand();
        if (m_stop.stop_requested() || m_discardCommands) {
            reply.fail(pnm::msg::ServiceError::Cancelled);
            return;
        }
        bool busy{};
        {
            std::scoped_lock lock{ m_commandMutex };
            busy = bool(m_command) || bool(m_programCommand);
        }
        if (busy) {
            reply.fail(pnm::msg::ServiceError::Busy);
            return;
        }
        auto command{ std::make_shared<Command>(value, std::move(reply)) };
        command->cancellation = pnm::msg::watch(command->reply, [weak = std::weak_ptr{ command }] {
            if (const auto pending{ weak.lock() }; pending && !pending->reply.pending()) {
                pending->stop.request_stop();
            }
        });
        if (value.operation == MotorOperation::Initialize && value.motor <= MotorId::All) {
            ++m_commandGeneration;
        }
        {
            std::scoped_lock lock{ m_commandMutex };
            m_command = std::move(command);
        }
        m_commandNotification.signal();
    }

    void Server::finishCommand()
    {
        std::shared_ptr<Command> command;
        {
            std::scoped_lock lock{ m_commandMutex };
            if (!m_command || !m_command->done.load()) {
                return;
            }
            command = std::exchange(m_command, {});
        }
        command->result.commandGeneration = m_commandGeneration;
        command->reply.respond(command->result);
    }

    void Server::cancelCommand()
    {
        std::shared_ptr<Command> command;
        std::shared_ptr<ProgramCommand> program;
        {
            std::scoped_lock lock{ m_commandMutex };
            command = m_command; program = m_programCommand;
        }
        if (program) program->stop.request_stop();
        if (command) {
            command->stop.request_stop();
        }
        m_commandNotification.signal();
    }

    void Server::discardQueuedCommands()
    {
        // Requests already waiting at the stop boundary must not re-enable the
        // controller afterwards. New operations require an explicit submission.
        m_discardCommands = true;
        m_commands.poll();
        m_discardCommands = false;
    }

    void Server::commandWorker(std::stop_token stop)
    {
        const std::stop_callback wake{ stop, [this] { m_commandNotification.signal(); } };
        while (!stop.stop_requested() && !m_stop.stop_requested()) {
            std::shared_ptr<Command> command;
            std::shared_ptr<ProgramCommand> program;
            {
                std::scoped_lock lock{ m_commandMutex };
                if (m_programCommand && !m_programCommand->started) {
                    program = m_programCommand; program->started = true;
                }
                if (m_command && !m_command->started) {
                    command = m_command;
                    command->started = true;
                }
            }
            if (program) {
                pnm::log::debug("Preparing job {}",program->request.job);
                const auto began=std::chrono::steady_clock::now();
                try { program->result = ::motion::trajectory::encode(
                    m_controller->prepareProgram(program->request, program->stop.get_token())); }
                catch (...) { program->result = ::motion::trajectory::encode(
                    ::motion::trajectory::Reply{.error=::motion::trajectory::Error::Fault}); }
                pnm::log::debug("Prepared job {} in {} us",program->request.job,
                  std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count());
                program->done.store(true); m_notification->signal();
                continue;
            }
            if (!command) {
                m_commandNotification.waitUntil(std::chrono::steady_clock::time_point::max());
                continue;
            }
            try {
                command->result = request(command->request, false, command->stop.get_token());
            } catch (...) {
                command->result.ok = false;
            }
            command->done.store(true);
            m_notification->signal();
        }
    }

    void Server::admit(const MotorGoal& goal, Execution execution)
    {
        if (m_stop.stop_requested() || !valid(goal) || goal.commandGeneration != m_commandGeneration ||
            !m_controller->ready() || !m_controller->enabled() ||
            (goal.motion == MotorMotion::Absolute && !m_controller->motor(id(goal.motor)).isReferenced())) {
            execution.reject();
            return;
        }
        if (execution.cancelRequested()) {
            execution.cancelled({ MotorOutcome::Stopped, {} });
            return;
        }
        auto slot{ std::ranges::find_if(m_jobs, [](const auto& job) { return !job; }) };
        if (slot == m_jobs.end()) {
            execution.fail(pnm::msg::ActionError::Busy);
            return;
        }
        auto finished{ std::make_shared<Finished>() };
        auto cancellation{ pnm::msg::watch(execution,
                                           [notification = m_notification] { notification->signal(); }) };
        std::function<void()> completed{ [finished, notification = m_notification] {
            finished->ready.store(true);
            notification->signal();
        } };
        if (goal.job) {
            const auto owner = m_controller->reserveMotor(goal.job, id(goal.motor));
            if (owner.error != ::motion::trajectory::Error::None) {
                execution.reject({.outcome=MotorOutcome::Rejected,.final_state=progress(goal.motor),.busy_job=owner.busy_job}); return;
            }
        } else if (m_controller->trajectorySnapshot().family != Family::None &&
                   m_controller->trajectorySnapshot().family != Family::Motor) {
            execution.fail(pnm::msg::ActionError::Busy); return;
        }
        if (!execution.accept()) {
            m_controller->releaseJob(goal.job); return;
        }
        slot->emplace(Job{ goal.motor, goal.job, std::move(execution), finished, {}, std::move(cancellation) });
        try {
            const auto motor{ id(goal.motor) };
            if (goal.motion == MotorMotion::Home) {
                (*slot)->result = m_controller->reference(motor,
                                                          goal.velocity * 1_rpm,
                                                          goal.latch_velocity * 1_rpm,
                                                          goal.timeout * 1_s,
                                                          std::move(completed));
            }
            else {
                const auto move{ goal.motion == MotorMotion::Relative ? &Controller::moveRel
                                                                      : &Controller::moveAbs };
                (*slot)->result = (m_controller->*move)(motor,
                                                        goal.position * 1_deg,
                                                        goal.velocity * 1_rpm,
                                                        goal.acceleration * 1_deg_s2,
                                                        goal.deceleration * 1_deg_s2,
                                                        goal.jerk * 1_deg_s3,
                                                        static_cast<BufferMode>(goal.buffer),
                                                        goal.timeout * 1_s,
                                                        std::move(completed));
            }
        } catch (...) {
            m_controller->stop(id(goal.motor));
            (*slot)->execution.abort({ MotorOutcome::Faulted, progress(goal.motor) });
            m_controller->releaseJob(goal.job);
            slot->reset();
        }
    }

    void Server::dispatchJobs(bool feedback)
    {
        for (auto& job : m_jobs) {
            if (!job) {
                continue;
            }
            if (!job->finished->ready.load() && job->execution.cancelRequested()) {
                ++m_commandGeneration;
                // Cancellation is an axis stop, including its buffered successor.
                // Join before reporting a terminal result; other motors keep running.
                m_controller->stop(id(job->motor));
            }
            if (job->finished->ready.load()) {
                auto outcome{ StepperMotor::Result::Faulted };
                try {
                    outcome = job->result.get();
                } catch (...) {
                }
                const MotorResult result{ static_cast<MotorOutcome>(outcome), progress(job->motor) };
                if (outcome == StepperMotor::Result::Completed) {
                    job->execution.succeed(result);
                }
                else if (outcome == StepperMotor::Result::Stopped) {
                    job->execution.cancelled(result);
                }
                else {
                    job->execution.abort(result);
                }
                m_controller->releaseJob(job->id);
                job.reset();
            }
            else if (feedback) {
                job->execution.feedback(progress(job->motor));
            }
        }
    }

    void Server::run(std::stop_token stop)
    {
        m_stop = stop;
        const auto wake{ [notification = m_notification] { notification->signal(); } };
        const auto commands{ pnm::msg::watch(m_commands, wake) };
        const auto urgent{ pnm::msg::watch(m_urgent, wake) };
        const auto actions{ pnm::msg::watch(m_motions, wake) };
        const auto trajectories{ pnm::msg::watch(m_trajectories, wake) };
        const auto trajectoryControl{pnm::msg::watch(m_trajectoryControl, wake)};
        const auto programs{pnm::msg::watch(m_programs, wake)};
        const std::stop_callback cancellation{ stop, [this, &wake] {
            cancelCommand();
            wake();
        } };
        // ARM finite preparation peaked at 11,020 bytes; 16 KiB preserves
        // more than 25% headroom. Hosted Debug keeps its larger allowance.
        auto worker{ runtime::thread::create_jthread(
          { .name = "motor commands", .priority = 2,
            .stack_size = sizeof(void*) == 4 ? 16UZ * 1024UZ : 24UZ * 1024UZ },
          [this](std::stop_token token) { commandWorker(token); }) };
        const auto drainCommand{ [&] {
            cancelCommand();
            worker.request_stop();
            m_commandNotification.signal();
            worker.join();
            finishCommand(); finishProgramCommand();
            std::scoped_lock lock{ m_commandMutex };
            m_command.reset(); m_programCommand.reset();
        } };
        auto feedback{ std::chrono::steady_clock::now() };
        try {
            while (!stop.stop_requested()) {
                if(m_peerLost.exchange(false)){
                    ++m_commandGeneration;cancelCommand();m_controller->connectionLost();
                }
                m_trajectoryControl.poll();
                m_urgent.poll();
                finishProgramCommand();
                dispatchPrograms();
                dispatchJobs(false);
                finishCommand();
                if(m_peerStopping.load() && m_controller->trajectorySnapshot().family==Family::None)break;
                m_commands.poll();
                m_trajectories.poll();
                m_motions.poll();
                m_programs.poll();
                const auto now{ std::chrono::steady_clock::now() };
                dispatchJobs(now >= feedback);
                if (now >= feedback) {
                    m_state.publish(snapshot());
                    // Publishing can preempt this worker while ICC serializes
                    // the snapshot. Do not catch up an already expired period
                    // in a tight loop and starve diagnostics/logging workers.
                    feedback = std::chrono::steady_clock::now() + MOTOR_STATE_PERIOD;
                }
                m_notification->waitUntil(feedback);
            }
        } catch (...) {
            m_controller->disable();
            drainCommand();
            m_motions.close();
            m_commands.close();
            m_trajectories.close();
            m_trajectoryControl.close(); m_programs.close();
            m_urgent.close();
            throw;
        }
        m_commands.close();
        m_trajectories.close();
        m_trajectoryControl.close(); m_programs.requestStop();
        m_urgent.close();
        m_motions.requestStop();
        m_controller->disable();
        drainCommand();
        dispatchJobs(false);
        dispatchPrograms();
        m_programs.close();
        m_motions.close();
    }
}
