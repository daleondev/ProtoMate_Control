#include "Client.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace ui::motion
{
    using namespace std::chrono_literals;
    void Client::setObserver(std::function<void()> observer) { m_observer = std::move(observer); }
    std::chrono::steady_clock::time_point Client::receivedAt() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_receivedAt;
    }
    unsigned Client::copyJobs(unsigned offset, std::span<JobInfo> output) const
    {
        std::scoped_lock lock{ m_mutex };
        // Select newest IDs without allocating or duplicating the history pool.
        std::uint64_t before = UINT64_MAX;
        unsigned written{};
        for (unsigned n = 0; n < offset + output.size(); ++n) {
            const JobInfo* selected{};
            auto consider = [&](const JobInfo& j) {
                if (j.id < before && (!selected || j.id > selected->id))
                    selected = &j;
            };
            for (unsigned i = 0; i < m_history->count; ++i)
                consider(m_history->jobs[i]);
            for (const auto& j : m_jobs)
                consider(j->info);
            if (!selected)
                break;
            before = selected->id;
            if (n >= offset)
                output[written++] = *selected;
        }
        return written;
    }
    bool Client::requestAsync(const ::motion::MotorRequest& value, Completion completion)
    {
        std::scoped_lock lock{ m_mutex };
        if (m_stopped || !m_connected)
            return false;
        if (value.operation == ::motion::MotorOperation::Disable) {
            if (m_disable)
                return false;
            for (auto& call : m_calls)
                if (call) {
                    call->cancelled = true;
                    call->readiness = {};
                    call->handle.reset();
                }
            m_disable.emplace();
            m_disable->request = value;
            m_disable->completion = std::move(completion);
            m_notification->signal();
            return true;
        }
        if (m_disable)
            return false;
        for (auto& slot : m_calls)
            if (!slot) {
                slot.emplace();
                slot->request = value;
                slot->completion = std::move(completion);
                m_notification->signal();
                return true;
            }
        return false;
    }
    void Client::stopAsync(std::uint8_t axes, bool abort)
    {
        std::array<std::optional<std::stop_source>, 8> sources;
        unsigned n{};
        {
            std::scoped_lock lock{ m_mutex };
            axes &= 7;
            for (const auto& job : m_jobs)
                if (!terminal(job->info.state) && (job->info.axes & axes)) {
                    job->info.state = JobState::Stopping;
                    sources[n++] = job->stop;
                    if (job->handle)
                        job->handle->requestCancel();
                    if (job->program)
                        job->program->requestCancel();
                }
            if (m_connected) {
                m_stopAxes |= axes;
                if (abort)
                    m_abortAxes |= axes;
            }
        }
        for (unsigned i = 0; i < n; ++i)
            sources[i]->request_stop();
        m_notification->signal();
    }
    void Client::cancelAsync(std::uint64_t id)
    {
        std::optional<std::stop_source> source;
        std::uint8_t axes{};
        {
            std::scoped_lock lock{ m_mutex };
            const auto found =
              std::find_if(m_jobs.begin(), m_jobs.end(), [&](const auto& j) { return j->info.id == id; });
            if (found == m_jobs.end() || terminal((*found)->info.state))
                return;
            auto& job = **found;
            source = job.stop;
            job.info.state = JobState::Stopping;
            if (job.handle)
                job.handle->requestCancel();
            if (job.program)
                job.program->requestCancel();
            if (job.reservation)
                axes = job.info.axes;
        }
        source->request_stop();
        if (axes)
            stopAsync(axes);
        m_notification->signal();
    }
    void Client::superviseJog(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        m_inputJob = id;
        m_inputDeadline = std::chrono::steady_clock::now() + 200ms;
        m_notification->signal();
    }
    void Client::renewJogInput(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        if (m_inputJob == id && std::chrono::steady_clock::now() < m_inputDeadline)
            m_inputDeadline = std::chrono::steady_clock::now() + 200ms;
    }
    void Client::releaseJogInput()
    {
        std::uint64_t id{};
        {
            std::scoped_lock lock{ m_mutex };
            id = std::exchange(m_inputJob, 0);
        }
        if (id)
            cancelAsync(id);
    }
    void Client::serviceControls()
    {
        std::array<Completion, 5> callbacks;
        std::array<std::expected<::motion::MotorResponse, pnm::msg::ServiceError>, 5> results;
        unsigned count{};
        std::uint64_t expired{};
        {
            std::scoped_lock lock{ m_mutex };
            if (m_inputJob && std::chrono::steady_clock::now() >= m_inputDeadline)
                expired = std::exchange(m_inputJob, 0);
            auto service = [&](std::optional<ControlCall>& slot, bool urgent) {
                if (!slot)
                    return;
                if (slot->cancelled) {
                    results[count] = std::unexpected(pnm::msg::ServiceError::Cancelled);
                }
                else if (!m_connected || m_stopped) {
                    results[count] = std::unexpected(pnm::msg::ServiceError::TransportError);
                }
                else {
                    if (!slot->handle) {
                        slot->handle.emplace((urgent ? m_urgent : m_commands).request(slot->request, 3s));
                        slot->readiness =
                          pnm::msg::watch(*slot->handle, [n = m_notification] { n->signal(); });
                    }
                    if (!slot->handle->ready())
                        return;
                    results[count] = slot->handle->get();
                    if (results[count])
                        m_commandGeneration =
                          std::max(m_commandGeneration.value_or(0), results[count]->commandGeneration);
                }
                callbacks[count++] = std::move(slot->completion);
                slot.reset();
            };
            service(m_disable, true);
            for (auto& slot : m_calls)
                service(slot, false);
            if (m_stopCall && m_stopCall->ready()) {
                const auto response = m_stopCall->get();
                // Local job completion still comes from its action; an ACK is not proof of rest.
                const auto decoded = response ? ::motion::trajectory::decodeReply(*response)
                                              : std::unexpected(::motion::trajectory::Error::Unknown);
                if (!decoded || decoded->error != ::motion::trajectory::Error::None)
                    for (auto& job : m_jobs)
                        if (job->info.state == JobState::Stopping) {
                            job->info.state = JobState::Unknown;
                            job->info.error = decoded ? decoded->error : decoded.error();
                        }
                m_stopReady = {};
                m_stopCall.reset();
            }
            if (!m_connected) {
                m_stopAxes = m_abortAxes = 0;
                m_stopReady = {};
                m_stopCall.reset();
            }
            if (m_connected && m_boot && m_stopAxes && !m_stopCall) {
                namespace tr = ::motion::trajectory;
                tr::Request request{ .operation = m_abortAxes ? tr::Operation::Abort : tr::Operation::Stop,
                                     .boot = m_boot };
                request.axes = m_abortAxes ? m_abortAxes : m_stopAxes;
                m_stopCall.emplace(m_control.request(tr::encode(request), 3s));
                m_stopReady = pnm::msg::watch(*m_stopCall, [n = m_notification] { n->signal(); });
                m_stopAxes &= ~request.axes;
                m_abortAxes &= ~request.axes;
            }
        }
        if (expired)
            cancelAsync(expired);
        for (unsigned i = 0; i < count; ++i)
            if (callbacks[i])
                callbacks[i](std::move(results[i]));
        if (m_observer)
            m_observer();
    }
}
