#pragma once

#include "hal/hal.hpp"
#include <cstdint>

namespace hal::stm32
{
    // HSEM allocation: 0 boot; 1/2 core doorbells; 3 RTC; 4 RNG; 5 GPIO configuration.
    // The caller must also prevent reentry on its own core (interrupt guard or
    // local mutex). Bounded acquisition fails if a stopped peer owns the lock.
    class HardwareSemaphore final
    {
      public:
        explicit HardwareSemaphore(std::uint32_t id) noexcept : m_id(id)
        {
            for (std::uint32_t attempt{}; attempt < 100'000; ++attempt) {
                if (HAL_HSEM_FastTake(id) == HAL_OK) {
                    __DMB();
                    m_owned = true;
                    break;
                }
            }
        }
        ~HardwareSemaphore()
        {
            if (m_owned) { __DMB(); HAL_HSEM_Release(m_id, 0); }
        }
        HardwareSemaphore(const HardwareSemaphore&) = delete;
        HardwareSemaphore& operator=(const HardwareSemaphore&) = delete;
        explicit operator bool() const noexcept { return m_owned; }
      private:
        std::uint32_t m_id;
        bool m_owned{};
    };
}
