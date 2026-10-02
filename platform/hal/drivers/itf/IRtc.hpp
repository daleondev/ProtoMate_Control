#pragma once

#include "hal/utilities/Result.hpp"

#include <cstdint>

namespace hal
{
    class IRtc
    {
      public:
        static constexpr std::uint32_t NANOSECONDS_PER_SECOND{ 1'000'000'000U };

        struct Timestamp
        {
            // UTC Unix time split into whole seconds and a fractional second.
            std::int64_t seconds_since_epoch;
            std::uint32_t nanoseconds;

            constexpr bool operator==(const Timestamp&) const = default;
        };

        virtual ~IRtc() = default;

        IRtc(const IRtc&) = delete;
        IRtc& operator=(const IRtc&) = delete;
        IRtc(IRtc&&) = delete;
        IRtc& operator=(IRtc&&) = delete;

        [[nodiscard]] virtual auto getTime() const noexcept -> util::Result<Timestamp> = 0;

      protected:
        IRtc() = default;
    };
}
