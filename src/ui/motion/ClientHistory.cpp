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
        constexpr std::size_t HISTORY_ALIGNMENT{ 32 }, HISTORY_STORAGE_BYTES{ 8192 };
        // Completed jobs are POD records. Keep them in the existing motion
        // reservation; only live handles/cancellation state use internal RAM.
        // Linker-reserved history arena; atomic claim guarantees exclusive client ownership.
        // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
        alignas(HISTORY_ALIGNMENT) __attribute__((
          section(".motion_sdram"))) std::array<std::byte, HISTORY_STORAGE_BYTES> history_storage{};
        // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
        // Linker-reserved history arena; atomic claim guarantees exclusive client ownership.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_bool history_claimed{};
#endif
    }
    auto Client::reserveHistory() -> std::unique_ptr<History, ReleaseHistory>
    {
#if defined(CORE_CM7)
        static_assert(sizeof(History) <= sizeof(history_storage));
        if (history_claimed.exchange(true)) {
            throw std::logic_error("motion history already owned");
        }
        // Construct the owned history in its aligned, linker-reserved arena.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* history{ std::construct_at(reinterpret_cast<History*>(history_storage.data())) };
        return std::unique_ptr<History, ReleaseHistory>{ history };
#else
        return std::unique_ptr<History, ReleaseHistory>{ new History{} };
#endif
    }
    void Client::ReleaseHistory::operator()(History* history) const noexcept
    {
#if defined(CORE_CM7)
        std::destroy_at(history);
        history_claimed = false;
#else
        std::default_delete<History>{}(history);
#endif
    }
    Client::~Client() = default;
    void Client::describe(std::uint64_t id, Requested request)
    {
        std::scoped_lock lock{ m_mutex };
        findLocked(id)->info.requested = request;
    }
    void Client::setPlannedDuration(std::uint64_t id, double seconds)
    {
        std::scoped_lock lock{ m_mutex };
        findLocked(id)->info.planned_seconds = seconds;
    }
    void Client::retireJobsLocked()
    {
        std::erase_if(m_jobs, [this](const auto& job) {
            if (!terminal(job->info.state) || job->handle || job->program || job->reservation ||
                job->preparing) {
                return false;
            }
            m_history->jobs[m_history->count++] = job->info;
            return true;
        });
        std::sort(m_history->jobs.begin(),
                  m_history->jobs.begin() + m_history->count,
                  [](const auto& a, const auto& b) { return a.id < b.id; });
    }
    void Client::finishPlanning(std::uint64_t id)
    {
        std::scoped_lock lock{ m_mutex };
        findLocked(id)->preparing = false;
        retireJobsLocked();
    }
}
