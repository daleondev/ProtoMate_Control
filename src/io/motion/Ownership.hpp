#pragma once

#include "common/motion/Trajectory.hpp"
#include "common/motion/OperatingEnvelope.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>

namespace io::motion
{
    // Controller-lock protected. Reservations cover preparation, execution and
    // cleanup. Only the completion owner may release a stopping reservation.
    class Ownership final
    {
      public:
        using Clock = std::chrono::steady_clock;
        struct Entry
        {
            std::uint64_t job{}, token{}, configuration{};
            ::motion::Family family{};
            std::uint8_t axes{};
            ::motion::JobState state{::motion::JobState::Planning};
            Clock::time_point deadline{};
            std::array<std::uint64_t, 3> revisions{};
        };
        struct Admission
        {
            ::motion::trajectory::Error error{};
            std::uint64_t token{}, busy_job{};
        };
        Admission acquire(std::uint64_t job, ::motion::Family family, std::uint8_t axes,
                          std::uint64_t configuration, Clock::time_point now)
        {
            using enum ::motion::Family;
            using E = ::motion::trajectory::Error;
            if (!job || family == None || family > Robot || !axes || axes > 7 ||
                (family == Robot && axes != 7) || (family != Robot && std::popcount(axes) != 1))
                return {E::InvalidInput};
            for (const auto& e : m_entries) {
                if (!e.job) continue;
                if (e.job == job) return {E::Stale};
                if (e.family != family || family == Robot ||
                    (family != Motor && (e.axes & axes)))
                    return {E::Busy, 0, e.job};
            }
            auto slot = std::ranges::find_if(m_entries, [](const auto& e) { return !e.job; });
            if (slot == m_entries.end()) return {E::Capacity};
            *slot = {.job=job, .token=++m_next, .configuration=configuration, .family=family,
                     .axes=axes, .deadline=now + std::chrono::milliseconds(::motion::operating::OWNER_LEASE_MS),
                     .revisions=m_revisions};
            return {E::None, slot->token};
        }
        Entry* find(std::uint64_t job) noexcept
        {
            if (!job) return nullptr;
            const auto i = std::ranges::find(m_entries, job, &Entry::job);
            return i == m_entries.end() ? nullptr : &*i;
        }
        const Entry* find(std::uint64_t job) const noexcept
        { return const_cast<Ownership*>(this)->find(job); }
        bool renew(std::uint64_t job, Clock::time_point now) noexcept
        {
            auto* e = find(job);
            if (!e || now >= e->deadline || e->state >= ::motion::JobState::Stopping) return false;
            e->deadline = now + std::chrono::milliseconds(::motion::operating::OWNER_LEASE_MS);
            return true;
        }
        void release(std::uint64_t job) noexcept
        {
            if (auto* e = find(job)) { changed(e->axes); *e = {}; }
        }
        void changed(std::uint8_t axes) noexcept
        { for (unsigned i=0; i<3; ++i) if (axes & (1U<<i)) ++m_revisions[i]; }
        void expire(Clock::time_point now) noexcept
        { for(auto& e:m_entries)if(e.job)e.deadline=std::min(e.deadline,now); }
        void invalidate() noexcept
        {
            // Preserve ownership until callbacks and cleanup finish.
            changed(7);
            for (auto& e : m_entries) if (e.job) e.state = ::motion::JobState::Stopping;
        }
        ::motion::Family family() const noexcept
        {
            for (const auto& e : m_entries) if (e.job) return e.family;
            return ::motion::Family::None;
        }
        std::array<std::uint64_t, 3> owners() const noexcept
        {
            std::array<std::uint64_t, 3> result{};
            for (const auto& e : m_entries)
                if (e.job) for (unsigned i=0; i<3; ++i)
                    if ((e.axes & (1U<<i)) && !result[i]) result[i] = e.job;
            return result;
        }
        const auto& revisions() const noexcept { return m_revisions; }
        const auto& entries() const noexcept { return m_entries; }
      private:
        std::array<Entry, 8> m_entries{};
        std::array<std::uint64_t, 3> m_revisions{1,1,1};
        std::uint64_t m_next{};
    };
}
