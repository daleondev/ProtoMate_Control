#include "Client.hpp"
#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>

namespace ui::motion
{
    namespace
    {
#if defined(CORE_CM7)
        // Completed jobs are POD records. Keep them in the existing motion
        // reservation; only live handles/cancellation state use internal RAM.
        alignas(32) __attribute__((section(".motion_sdram"))) std::byte historyStorage[8192];
        std::atomic_bool historyClaimed{};
#endif
    }
    auto Client::reserveHistory() -> std::unique_ptr<History, ReleaseHistory>
    {
#if defined(CORE_CM7)
        static_assert(sizeof(History) <= sizeof(historyStorage));
        if (historyClaimed.exchange(true)) throw std::logic_error("motion history already owned");
        return std::unique_ptr<History, ReleaseHistory>{std::construct_at(reinterpret_cast<History*>(historyStorage))};
#else
        return std::unique_ptr<History, ReleaseHistory>{new History{}};
#endif
    }
    void Client::ReleaseHistory::operator()(History* history) const noexcept
    {
#if defined(CORE_CM7)
        std::destroy_at(history);
        historyClaimed=false;
#else
        delete history;
#endif
    }
    Client::~Client() = default;
    void Client::setPlannedDuration(std::uint64_t id,double seconds)
    {
        std::scoped_lock lock{m_mutex};
        findLocked(id)->info.planned_seconds=seconds;
    }
    void Client::retireJobsLocked()
    {
        std::erase_if(m_jobs,[this](const auto& job) {
            if (!terminal(job->info.state) || job->handle || job->program || job->reservation || job->preparing)
                return false;
            m_history->jobs[m_history->count++]=job->info;
            return true;
        });
        std::sort(m_history->jobs.begin(),m_history->jobs.begin()+m_history->count,
                  [](const auto& a,const auto& b){return a.id<b.id;});
    }
    void Client::finishPlanning(std::uint64_t id)
    {
        std::scoped_lock lock{m_mutex};
        findLocked(id)->preparing=false;
        retireJobsLocked();
    }
}
