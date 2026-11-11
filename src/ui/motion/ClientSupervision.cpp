#include "Client.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>

namespace ui::motion
{
    bool Client::remoteAuthorizedLocked(std::uint64_t source) const
    {
        if (!source) return true;
        if (!m_remotePermission) return false;
        const auto now{ std::chrono::steady_clock::now() };
        for (const auto& remote : m_remote)
            if (remote.source == source) return remote.active && now < remote.deadline;
        return false;
    }
    bool Client::remoteAuthorized(std::uint64_t source) const
    { std::scoped_lock lock{m_mutex}; return source && remoteAuthorizedLocked(source); }
    bool Client::remotePermission() const
    { std::scoped_lock lock{m_mutex}; return m_remotePermission; }
    void Client::remotePermission(bool enabled)
    {
        std::scoped_lock lock{m_mutex};
        m_remotePermission = enabled && m_connected && !m_stopped;
        if (!m_remotePermission) for (auto& remote : m_remote) remote.active = false;
        m_notification->signal();
    }
    std::uint64_t Client::openRemote()
    {
        std::scoped_lock lock{m_mutex};
        if (!m_remotePermission || !m_nextRemote || m_stopped) return 0;
        for (auto& remote : m_remote) if (!remote.active) {
            remote = {m_nextRemote++, std::chrono::steady_clock::now() + REMOTE_TIMEOUT, true};
            m_notification->signal();
            return remote.source;
        }
        return 0;
    }
    void Client::closeRemote(std::uint64_t source)
    {
        std::scoped_lock lock{m_mutex};
        for (auto& remote : m_remote) if (remote.source == source) remote.active = false;
        m_notification->signal();
    }
    bool Client::renewRemote(std::uint64_t source, std::span<const std::uint64_t> heldJobs)
    {
        std::scoped_lock lock{m_mutex};
        if (!source || !remoteAuthorizedLocked(source) || heldJobs.size() > MAX_JOBS) return false;
        const auto now{ std::chrono::steady_clock::now() };
        for (const auto id : heldJobs) {
            const auto found{ std::ranges::find_if(m_jobs, [=](const auto& job) {return job->info.id == id && job->info.source == source;}) };
            // An expired gesture cannot be revived even if the worker has not
            // yet processed its deadline. Completed jobs may leave the list.
            if (found == m_jobs.end() || !(*found)->inputDeadline || now >= *(*found)->inputDeadline) return false;
        }
        for (auto& remote : m_remote) if (remote.source == source) remote.deadline = now + REMOTE_TIMEOUT;
        for (auto& job : m_jobs) if (job->info.source == source && job->inputDeadline) {
            const bool held{ std::ranges::find(heldJobs, job->info.id) != heldJobs.end() };
            if (now < *job->inputDeadline) job->inputDeadline = held ? now + REMOTE_TIMEOUT : now;
        }
        m_notification->signal();
        return true;
    }
    void Client::checkRawOwnerLocked(std::uint8_t axes, std::uint64_t source) const
    {
        for (const auto& job : m_jobs) {
            const bool cleaned{ terminal(job->info.state) && !job->handle && !job->program && !job->reservation && !job->preparing };
            if (!cleaned && job->info.family == ::motion::Family::Motor && (job->info.axes & axes) && job->info.source != source)
                throw std::runtime_error(std::format("Busy: raw queue belongs to another source (job #{}); stop or wait", job->info.id));
        }
    }
    void Client::superviseJog(std::uint64_t id)
    {
        std::scoped_lock lock{m_mutex};
        auto job{findLocked(id)};
        if (job->info.source || terminal(job->info.state) || job->info.state == JobState::Stopping) return;
        job->inputDeadline = std::chrono::steady_clock::now() + INPUT_TIMEOUT;
        m_notification->signal();
    }
    void Client::renewJogInput(std::uint64_t id)
    {
        std::scoped_lock lock{m_mutex};
        const auto now{std::chrono::steady_clock::now()};
        for (auto& job : m_jobs) if (job->info.id == id && !job->info.source && job->inputDeadline && now < *job->inputDeadline)
            job->inputDeadline = now + INPUT_TIMEOUT;
    }
    void Client::releaseJogInput()
    {
        std::array<std::uint64_t, MAX_JOBS> ids{};
        unsigned count{};
        {
            std::scoped_lock lock{m_mutex};
            for (auto& job : m_jobs) if (!job->info.source && job->inputDeadline) {
                job->inputDeadline.reset(); ids[count++] = job->info.id;
            }
        }
        for (unsigned i{}; i < count; ++i) cancelAsync(ids[i]);
    }
    void Client::expireInputs()
    {
        std::array<std::uint64_t, MAX_JOBS> ids{};
        unsigned count{};
        {
            std::scoped_lock lock{m_mutex};
            const auto now{std::chrono::steady_clock::now()};
            for (auto& remote : m_remote) if (remote.active && now >= remote.deadline) remote.active = false;
            for (auto& job : m_jobs) {
                const bool expired{ (job->info.source && !remoteAuthorizedLocked(job->info.source)) ||
                    (job->inputDeadline && now >= *job->inputDeadline) };
                if (terminal(job->info.state) || job->info.state == JobState::Stopping) job->inputDeadline.reset();
                else if (expired) { job->inputDeadline.reset(); ids[count++] = job->info.id; }
            }
        }
        // Cancellation uses the existing action/urgent paths. Raw motor actions
        // abort, while axis/robot actions select their local stopping branch.
        for (unsigned i{}; i < count; ++i) cancelAsync(ids[i]);
    }
}
