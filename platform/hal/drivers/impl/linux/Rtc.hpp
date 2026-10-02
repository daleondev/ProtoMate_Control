#pragma once

#include "hal/drivers/itf/IRtc.hpp"

namespace hal
{
    class Rtc final : public IRtc
    {
      public:
        Rtc() = default;
        ~Rtc() override = default;

        Rtc(const Rtc&) = delete;
        Rtc& operator=(const Rtc&) = delete;
        Rtc(Rtc&&) = delete;
        Rtc& operator=(Rtc&&) = delete;

        [[nodiscard]] auto getTime() const noexcept -> util::Result<Timestamp> override;
    };
}
