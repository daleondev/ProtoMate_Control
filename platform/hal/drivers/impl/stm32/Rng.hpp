#pragma once

#include "hal/drivers/itf/IRng.hpp"
#include "hal/hal.hpp"
#include <mutex>

namespace hal
{
    class Rng final : public IRng
    {
      public:
        // M7 initializes the peripheral at boot; each core has a private HAL handle.
        explicit Rng(RNG_HandleTypeDef& handle) : m_handle(handle) {}
        auto generate() noexcept -> util::Result<Value> override;
      private:
        RNG_HandleTypeDef& m_handle;
        std::mutex m_mutex;
    };
}
