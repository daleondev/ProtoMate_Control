#pragma once

#include "hal/drivers/itf/IRng.hpp"

namespace hal
{
    class Rng final : public IRng
    {
      public:
        Rng() = default;
        ~Rng() override = default;

        Rng(const Rng&) = delete;
        Rng& operator=(const Rng&) = delete;
        Rng(Rng&&) = delete;
        Rng& operator=(Rng&&) = delete;

        [[nodiscard]] auto generate() noexcept -> util::Result<Value> override;
    };
}
