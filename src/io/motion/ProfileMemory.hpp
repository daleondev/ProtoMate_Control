#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace io::motion
{
    // Three nominal profiles and two immutable stop banks PER AXIS. The same
    // pool supports one coordinated group or three independent groups; neither
    // admission order nor another axis's turnover can consume its reservations.
    class ProfileMemory final : public std::pmr::memory_resource
    {
      public:
        static constexpr unsigned NOMINAL_SLOTS{ 3 }, STOP_SLOTS{ 6 },
          TOTAL_SLOTS{ NOMINAL_SLOTS + STOP_SLOTS };
        static constexpr std::size_t STORAGE_BUDGET{ 26176 };
        static constexpr std::size_t PROFILE_BYTES{ 3072 };
        static constexpr std::size_t ALIGNMENT{ 32 };

      private:
        template<std::size_t N>
        struct alignas(ALIGNMENT) Slot
        {
            std::array<std::byte, N> bytes;
        };
        // Only the three nominal profiles own geometric curves. Stopping
        // banks share them, so retain the original smaller six reservations.
        static constexpr std::size_t STOP_BYTES{ 2816 };
        std::array<Slot<STOP_BYTES>, STOP_SLOTS> m_stops{};
        std::array<Slot<PROFILE_BYTES>, NOMINAL_SLOTS> m_nominals{};
        void* slot(unsigned i) noexcept
        {
            return i < STOP_SLOTS ? m_stops[i].bytes.data() : m_nominals[i - STOP_SLOTS].bytes.data();
        }
        std::atomic_uint m_used{};
        void* do_allocate(std::size_t bytes, std::size_t alignment) override
        {
            if (alignment > ALIGNMENT || bytes > PROFILE_BYTES) {
                throw std::bad_alloc();
            }
            for (unsigned i{ 0 }; i < TOTAL_SLOTS; ++i) {
                if (i < STOP_SLOTS && bytes > STOP_BYTES) {
                    continue;
                }
                const auto bit{ 1U << i };
                auto old{ m_used.load(std::memory_order_relaxed) };
                while (!(old & bit)) {
                    if (m_used.compare_exchange_weak(old, old | bit, std::memory_order_acquire)) {
                        return slot(i);
                    }
                }
            }
            throw std::bad_alloc();
        }
        void do_deallocate(void* pointer, std::size_t /*__bytes*/, std::size_t /*__alignment*/) override
        {
            for (unsigned i{ 0 }; i < TOTAL_SLOTS; ++i) {
                if (pointer == slot(i)) {
                    m_used.fetch_and(~(1U << i), std::memory_order_release);
                    return;
                }
            }
            std::terminate();
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
        {
            return this == &other;
        }
    };
    static_assert(sizeof(ProfileMemory) <= ProfileMemory::STORAGE_BUDGET);
}
