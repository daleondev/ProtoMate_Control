#include "Client.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"

#include <algorithm>
#include <stdexcept>

namespace ui::motion
{
    namespace
    {
        using namespace std::chrono_literals;
        using namespace ::motion;
        const char* serviceError(pnm::msg::ServiceError error)
        {
            using enum pnm::msg::ServiceError;
            switch (error) {
                case Unavailable: return "IO motor service unavailable";
                case Busy: return "IO motor service busy";
                case Timeout: return "IO motor service timed out; operation outcome unknown";
                case Cancelled: return "motor request cancelled; operation outcome unknown";
                case HandlerFailed: return "IO motor service failed; operation outcome unknown";
                case TransportError: return "ICC communication failed; motor stop unconfirmed";
            }
            return "unknown motor service error";
        }
    }

    Client::Client(pnm::msg::Bus& bus)
      : m_commands{ bus.service<^^icc::Services::io_motor>() }
      , m_urgent{ bus.service<^^icc::Services::io_motor_stop>() }
      , m_motions{ bus.action<^^icc::Actions::io_motor_motion>() }
    {
        m_jobs.reserve(32);
    }

    MotorResponse Client::request(const MotorRequest& value, std::stop_token stop)
    {
        if (stop.stop_requested()) { throw std::runtime_error("UI controller is stopping"); }
        {
            std::scoped_lock lock{ m_mutex };
            if (m_stopped) { throw std::runtime_error("motor client is stopped"); }
        }
        const bool urgent{ value.operation == MotorOperation::Stop || value.operation == MotorOperation::Disable };
        auto call{ (urgent ? m_urgent : m_commands).request(value, 3s) };
        const std::stop_callback cancellation{ stop, [&call] { call.cancel(); } };
        auto result{ call.get() };
        if (!result) { throw std::runtime_error(serviceError(result.error())); }
        if (!result->ok) { throw std::runtime_error(result->message); }
        return std::move(*result);
    }

    std::uint64_t Client::submit(MotorGoal goal, std::stop_token stop)
    {
        // Snapshot and submission are distinct: IO rejects a plan overtaken by stop.
        const auto status{ request({}, stop) };
        if (!status.ready || !status.enabled) {
            throw std::runtime_error("run 'motor driver init' and 'motor enable' before moving");
        }
        goal.generation = status.generation;
        std::scoped_lock lock{ m_mutex };
        if (m_stopped || stop.stop_requested()) { throw std::runtime_error("UI controller is stopping"); }
        if (m_jobs.size() == 32) {
            const auto completed{ std::ranges::find_if(m_jobs, [](const auto& job) { return !job->handle; }) };
            if (completed == m_jobs.end()) { throw std::runtime_error("motor job registry is full"); }
            m_jobs.erase(completed);
        }
        auto job{ std::make_shared<Job>() };
        job->info = { .id = m_nextId++, .motor = goal.motor, .motion = goal.motion, .progress = {} };
        const std::weak_ptr<Job> weak{ job };
        // Reserve ownership before sending; nothing after a successful submission
        // may silently drop the only handle and cancel its motion.
        m_jobs.push_back(job);
        try {
            job->handle.emplace(m_motions.sendGoal(goal, { .accept_timeout = 3s }, {
                .on_accepted = [weak] {
                    if (auto current{ weak.lock() }) { current->info.state = JobState::Accepted; }
                },
                .on_feedback = [weak](const MotorProgress& progress) {
                    if (auto current{ weak.lock() }) { current->info.progress = progress; }
                },
                .on_result = [weak](pnm::msg::ActionResult<MotorResult> result) {
                    if (auto current{ weak.lock() }) {
                        if (!result) {
                            current->info.state = result.error() == pnm::msg::ActionError::Rejected ||
                                                  result.error() == pnm::msg::ActionError::Busy
                                                   ? JobState::Rejected : JobState::Unknown;
                            return;
                        }
                        current->info.progress = result->value.final_state;
                        if (result->status == pnm::msg::ActionStatus::Rejected) {
                            current->info.state = JobState::Rejected;
                            return;
                        }
                        if (result->status == pnm::msg::ActionStatus::Cancelled) {
                            current->info.state = JobState::Stopped;
                            return;
                        }
                        switch (result->value.outcome) {
                            case MotorOutcome::Completed: current->info.state = JobState::Completed; break;
                            case MotorOutcome::Stopped: current->info.state = JobState::Stopped; break;
                            case MotorOutcome::TimedOut: current->info.state = JobState::TimedOut; break;
                            case MotorOutcome::Rejected: current->info.state = JobState::Rejected; break;
                            case MotorOutcome::Faulted: current->info.state = JobState::Faulted; break;
                            default: current->info.state = JobState::Unknown; break;
                        }
                    }
                }
            }));
            job->readiness = pnm::msg::watch(*job->handle, [notification = m_notification] { notification->signal(); });
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
        std::vector<JobInfo> result;
        result.reserve(m_jobs.size());
        for (const auto& job : m_jobs) { result.push_back(job->info); }
        return result;
    }

    void Client::run(std::stop_token stop)
    {
        const std::stop_callback cancellation{ stop, [notification = m_notification] { notification->signal(); } };
        while (!stop.stop_requested()) {
            {
                std::scoped_lock lock{ m_mutex };
                for (const auto& job : m_jobs) {
                    if (job->handle && job->handle->poll()) {
                        job->readiness = {};
                        job->handle.reset();
                    }
                }
            }
            m_notification->waitUntil(std::chrono::steady_clock::time_point::max());
        }
        std::scoped_lock lock{ m_mutex };
        m_stopped = true;
        for (const auto& job : m_jobs) {
            if (job->handle) {
                job->handle->requestCancel();
                // A cancellation request is not proof of a stopped motor.
                if (!job->handle->poll()) { job->info.state = JobState::Unknown; }
                job->readiness = {};
                job->handle.reset();
            }
        }
    }
}
