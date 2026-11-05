#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace io::motion
{
    // Storage for one executor's two schedule banks and three nominal profiles.
    // Put this object in local SRAM at composition time, not in the AXI heap.
    // It must outlive the executor and every output holding one of its sequences.
    // Allocation is bounded, reusable and has no fallback to slower memory.
    class ProfileMemory final : public std::pmr::memory_resource
    {
      public:
        static constexpr std::size_t BANK_BYTES = 8448;
        static constexpr std::size_t PROFILE_BYTES = 2816;
        static constexpr std::size_t ALIGNMENT = 32;

      private:
        template<std::size_t N> struct alignas(ALIGNMENT) Slot { std::array<std::byte, N> bytes; };
        std::array<Slot<BANK_BYTES>, 2> m_banks;
        std::array<Slot<PROFILE_BYTES>, 3> m_profiles;
        std::atomic_uint m_used{};

        void* do_allocate(std::size_t bytes, std::size_t alignment) override
        {
            if (alignment > ALIGNMENT || bytes > BANK_BYTES)
                throw std::bad_alloc();
            // Keep the two large slots available for banks; small allocations
            // cannot consume them and starve a later command's preparation.
            const unsigned first = bytes > PROFILE_BYTES ? 0 : 2;
            const unsigned end = bytes > PROFILE_BYTES ? 2 : 5;
            for (unsigned i = first; i < end; ++i) {
                const auto bit = 1U << i;
                auto old = m_used.load(std::memory_order_relaxed);
                while (!(old & bit))
                    if (m_used.compare_exchange_weak(old, old | bit, std::memory_order_acquire))
                        return i < 2 ? m_banks[i].bytes.data() : m_profiles[i - 2].bytes.data();
            }
            throw std::bad_alloc();
        }
        void do_deallocate(void* pointer, std::size_t, std::size_t) override
        {
            for (unsigned i = 0; i < 5; ++i) {
                void* slot = i < 2 ? m_banks[i].bytes.data() : m_profiles[i - 2].bytes.data();
                if (pointer == slot) {
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
}
