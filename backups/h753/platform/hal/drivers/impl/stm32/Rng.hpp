#pragma once

#include "hal/drivers/itf/IRng.hpp"
#include "hal/hal.hpp"

#include <tx_api.h>

namespace hal
{
    class Rng final : public IRng
    {
      public:
        explicit Rng(RNG_HandleTypeDef& handle);
        ~Rng() override;

        Rng(const Rng&) = delete;
        Rng& operator=(const Rng&) = delete;
        Rng(Rng&&) = delete;
        Rng& operator=(Rng&&) = delete;

        [[nodiscard]] auto generate() noexcept -> util::Result<Value> override;

      private:
        RNG_HandleTypeDef& m_handle;
        TX_MUTEX m_mutex{};
    };
}
