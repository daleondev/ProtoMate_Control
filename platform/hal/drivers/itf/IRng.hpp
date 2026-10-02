#pragma once

#include "hal/utilities/Result.hpp"

#include <cstdint>

namespace hal
{
    class IRng
    {
      public:
        using Value = std::uint32_t;

        virtual ~IRng() = default;

        IRng(const IRng&) = delete;
        IRng& operator=(const IRng&) = delete;
        IRng(IRng&&) = delete;
        IRng& operator=(IRng&&) = delete;

        [[nodiscard]] virtual auto generate() noexcept -> util::Result<Value> = 0;

      protected:
        IRng() = default;
    };
}
