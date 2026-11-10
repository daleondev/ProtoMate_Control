#include "Client.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/catalog/topics.hpp"

#include <algorithm>
#include <stdexcept>

namespace ui::motion
{
    namespace
    {
        using namespace std::chrono_literals;
        using namespace ::motion;
        const char* service_error(pnm::msg::ServiceError error)
        {
            using enum pnm::msg::ServiceError;
            switch (error) {
                case Unavailable:
                    return "IO motor service unavailable";
                case Busy:
                    return "IO motor service busy";
                case Timeout:
                    return "IO motor service timed out; operation outcome unknown";
                case Cancelled:
                    return "motor request cancelled; operation outcome unknown";
                case HandlerFailed:
                    return "IO motor service failed; operation outcome unknown";
                case TransportError:
                    return "ICC communication failed; motor stop unconfirmed";
            }
            return "unknown motor service error";
        }
    }

    Client::Client(pnm::msg::Bus& bus)
      : m_history{ reserveHistory() }
      , m_commands{ bus.service<^^icc::Services::io_motor>() }
      , m_urgent{ bus.service<^^icc::Services::io_motor_stop>() }
      , m_motions{ bus.action<^^icc::Actions::io_motor_motion>() }
      , m_trajectory{ bus.service<^^icc::Services::io_trajectory>() }
      , m_control{ bus.service<^^icc::Services::io_trajectory_control>() }
      , m_programs{ bus.action<^^icc::Actions::io_motion>() }
      , m_stateSubscription{ bus.topic<^^icc::Topics::io_motor_state>().subscribe(
          [this](const MotorSnapshot& value) {
              std::scoped_lock lock{ m_mutex };
              if (!m_connected || m_stopped || (m_state && value.sequence <= m_state->sequence)) {
                  return;
              }
              m_boot = value.boot;
              m_state = value;
              m_receivedAt = std::chrono::steady_clock::now();
              // An older sample can arrive after a newer command acknowledgement.
              m_commandGeneration = std::max(m_commandGeneration.value_or(0), value.command_generation);
          },
          pnm::msg::SubscriptionOptions::latestOnly()) }
    {
        m_jobs.reserve(MAX_JOBS);
    }

    void Client::setConnected(bool connected)
    {
        std::array<std::optional<std::stop_source>, MAX_JOBS> cancel{};
        unsigned count{};
        {
            std::scoped_lock lock{ m_mutex };
            if (m_connected != connected) {
                m_state.reset();
                m_commandGeneration.reset();
            }
            m_connected = connected;
            if (!connected) {
                m_boot = 0;
                for (const auto& job : m_jobs) {
                    if (!terminal(job->info.state)) {
                        job->info.state = JobState::Unknown;
                        job->info.error = ::motion::trajectory::Error::Unknown;
                        if (count < cancel.size()) {
                            cancel[count++] = job->stop;
                        }
                        if (job->handle) {
                            job->handle->requestCancel();
                        }
                        if (job->program) {
                            job->program->requestCancel();
                        }
                    }
                }
                m_jobChanged.notify_all();
            }
        }
        // Stop callbacks can re-enter the client/planner; never invoke them under its mutex.
        for (auto& source : cancel) {
            if (source) {
                source->request_stop();
            }
        }
        m_notification->signal();
    }

    MotorSnapshot Client::state() const
    {
        std::scoped_lock lock{ m_mutex };
        if (m_stopped || !m_connected) {
            throw std::runtime_error("motor state unavailable: IO disconnected; motor stop unconfirmed");
        }
        if (!m_state) {
            throw std::runtime_error("motor state unavailable: awaiting IO telemetry");
        }
        if (std::chrono::steady_clock::now() - m_receivedAt > MOTOR_STATE_MAX_AGE) {
            throw std::runtime_error(
              "motor state stale: no fresh IO telemetry for 500 ms; motor stop unconfirmed");
        }
        return *m_state;
    }

    MotorResponse Client::request(const MotorRequest& value, const std::stop_token& stop)
    {
        if (stop.stop_requested()) {
            throw std::runtime_error("UI controller is stopping");
        }
        {
            std::scoped_lock lock{ m_mutex };
            if (m_stopped) {
                throw std::runtime_error("motor client is stopped");
            }
        }
        const bool urgent{ value.operation == MotorOperation::Stop ||
                           value.operation == MotorOperation::Disable };
        auto call{ (urgent ? m_urgent : m_commands).request(value, REQUEST_TIMEOUT) };
        const std::stop_callback cancellation{ stop, [&call] { call.cancel(); } };
        auto result{ call.get() };
        if (!result) {
            throw std::runtime_error(service_error(result.error()));
        }
        {
            std::scoped_lock lock{ m_mutex };
            if (m_connected && !m_stopped) {
                m_commandGeneration = std::max(m_commandGeneration.value_or(0), result->command_generation);
            }
        }
        if (!result->ok) {
            throw std::runtime_error(result->message);
        }
        return std::move(*result);
    }

    namespace
    {
        void motor_result(Client::JobInfo& info, const pnm::msg::ActionResult<MotorResult>& result)
        {
            using JobState = Client::JobState;
            if (!result) {
                info.state = result.error() == pnm::msg::ActionError::Rejected ||
                                 result.error() == pnm::msg::ActionError::Busy
                               ? JobState::Rejected
                               : JobState::Unknown;
                return;
            }
            info.progress = result->value.final_state;
            info.busy_job = result->value.busy_job;
            if (result->value.busy_job) {
                info.error = ::motion::trajectory::Error::Busy;
            }
            if (result->status == pnm::msg::ActionStatus::Rejected) {
                info.state = JobState::Rejected;
                return;
            }
            if (result->status == pnm::msg::ActionStatus::Cancelled) {
                info.state = JobState::Stopped;
                return;
            }
            switch (result->value.outcome) {
                case MotorOutcome::Completed:
                    info.state = JobState::Completed;
                    break;
                case MotorOutcome::Stopped:
                    info.state = JobState::Stopped;
                    break;
                case MotorOutcome::TimedOut:
                    info.state = JobState::TimedOut;
                    break;
                case MotorOutcome::Rejected:
                    info.state = JobState::Rejected;
                    break;
                case MotorOutcome::Faulted:
                    info.state = JobState::Faulted;
                    break;
                default:
                    info.state = JobState::Unknown;
                    break;
            }
        }
    }
    pnm::msg::ActionCallbacks<MotorProgress, MotorResult> Client::motorCallbacks(
      const std::weak_ptr<Job>& weak)
    {
        return { .on_accepted = [weak] {
            if (auto current{ weak.lock() }; current && current->info.state == JobState::Submitting) {
                current->info.state = JobState::Accepted;
            }
        }, .on_feedback = [weak](const MotorProgress& progress) {
            if (auto current{ weak.lock() }) {
                current->info.progress = progress;
            }
        }, .on_result = [weak](pnm::msg::ActionResult<MotorResult> result) {
            if (auto current{ weak.lock() }) {
                motor_result(current->info, result);
            }
        } };
    }
    std::uint64_t Client::submit(MotorGoal goal, const std::stop_token& stop)
    {
        std::scoped_lock lock{ m_mutex };
        if (goal.motor >= MotorId::All) {
            throw std::invalid_argument("motor must be m1, m2 or m3");
        }
        if (m_stopped || stop.stop_requested()) {
            throw std::runtime_error("UI controller is stopping");
        }
        if (!m_connected) {
            throw std::runtime_error("IO motor controller disconnected");
        }
        if (!m_commandGeneration) {
            throw std::runtime_error("awaiting IO motor state or command acknowledgement");
        }
        // IO authoritatively checks readiness and the stop generation. Cached
        // display state must not admit a move or delay one after an acknowledgement.
        goal.command_generation = *m_commandGeneration;
        auto job{ createLocked(Family::Motor, std::uint8_t(1U << unsigned(goal.motor))) };
        job->info.motor = goal.motor;
        job->info.motion = goal.motion;
        job->info.requested = { .values = { goal.position, 0, 0 },
                                .space =
                                  goal.motion == MotorMotion::Home ? RequestSpace::Home : RequestSpace::Shaft,
                                .relative = goal.motion == MotorMotion::Relative };
        goal.job = job->info.id;
        const std::weak_ptr<Job> weak{ job };
        try {
            job->handle.emplace(
              m_motions.sendGoal(goal, { .accept_timeout = REQUEST_TIMEOUT }, motorCallbacks(weak)));
            job->readiness =
              pnm::msg::watch(*job->handle, [notification = m_notification] { notification->signal(); });
        } catch (...) {
            job->handle.reset();
            job->info.state = JobState::Unknown;
            throw;
        }
        m_notification->signal();
        return job->info.id;
    }

    std::vector<Client::JobInfo> Client::jobs() const
    {
        std::scoped_lock lock{ m_mutex };
        std::vector<JobInfo> result{};
        result.reserve(m_jobs.size() + m_history->count);
        for (unsigned i{ 0 }; i < m_history->count; ++i) {
            result.push_back(m_history->jobs[i]);
        }
        for (const auto& job : m_jobs) {
            result.push_back(job->info);
        }
        std::ranges::sort(result, {}, &JobInfo::id);
        return result;
    }

    void Client::shutdownJobs()
    {
        std::array<std::optional<std::stop_source>, MAX_JOBS> cancel{};
        unsigned count{};
        {
            std::scoped_lock lock{ m_mutex };
            m_stopped = true;
            m_renewalReady = {};
            m_renewal.reset();
            for (const auto& job : m_jobs) {
                if (!terminal(job->info.state) && count < cancel.size()) {
                    cancel[count++] = job->stop;
                }
                if (job->program) {
                    job->program->requestCancel();
                    if (!job->program->poll()) {
                        job->info.state = JobState::Unknown;
                    }
                    job->readiness = {};
                    job->program.reset();
                }
                if (job->handle) {
                    job->handle->requestCancel();
                    // A cancellation request is not proof of a stopped motor.
                    if (!job->handle->poll()) {
                        job->info.state = JobState::Unknown;
                    }
                    job->readiness = {};
                    job->handle.reset();
                }
            }
            m_jobChanged.notify_all();
        }
        for (auto& source : cancel) {
            if (source) {
                source->request_stop();
            }
        }
    }
    void Client::run(const std::stop_token& stop)
    {
        const auto ready{ pnm::msg::watch(m_stateSubscription,
                                          [notification = m_notification] { notification->signal(); }) };
        const std::stop_callback cancellation{ stop,
                                               [notification = m_notification] { notification->signal(); } };
        while (!stop.stop_requested()) {
            m_stateSubscription.pollSome(1);
            {
                std::scoped_lock lock{ m_mutex };
                for (const auto& job : m_jobs) {
                    if (job->handle && job->handle->poll()) {
                        job->readiness = {};
                        job->handle.reset();
                        m_jobChanged.notify_all();
                    }
                    if (job->program && job->program->poll()) {
                        job->readiness = {};
                        job->program.reset();
                        job->reservation = 0;
                        m_jobChanged.notify_all();
                    }
                }
                retireJobsLocked();
            }
            serviceControls();
            renewJobs();
            auto deadline{ m_nextRenew };
            {
                std::scoped_lock lock{ m_mutex };
                if (m_inputJob) {
                    deadline = std::min(deadline, m_inputDeadline);
                }
            }
            m_notification->waitUntil(deadline);
        }
        shutdownJobs();
        serviceControls();
    }
}
