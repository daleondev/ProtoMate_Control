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
        static constexpr std::size_t PROFILE_BYTES = 2816;
        static constexpr std::size_t ALIGNMENT = 32;

      private:
        struct alignas(ALIGNMENT) Slot
        {
            std::array<std::byte, PROFILE_BYTES> bytes;
        };
        std::array<Slot, 9> m_slots{};
        std::atomic_uint m_used{};
        void* do_allocate(std::size_t bytes, std::size_t alignment) override
        {
            if (alignment > ALIGNMENT || bytes > PROFILE_BYTES) {
                throw std::bad_alloc();
            }
            for (unsigned i = 0; i < m_slots.size(); ++i) {
                const auto bit = 1U << i;
                auto old = m_used.load(std::memory_order_relaxed);
                while (!(old & bit)) {
                    if (m_used.compare_exchange_weak(old, old | bit, std::memory_order_acquire))
                        return m_slots[i].bytes.data();
                }
            }
            throw std::bad_alloc();
        }
        void do_deallocate(void* pointer, std::size_t, std::size_t) override
        {
            for (unsigned i = 0; i < m_slots.size(); ++i) {
                if (pointer == m_slots[i].bytes.data()) {
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
    static_assert(sizeof(ProfileMemory) <= 25408);
}
