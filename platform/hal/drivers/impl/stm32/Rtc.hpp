#pragma once

#include "hal/drivers/itf/IRtc.hpp"
#include "hal/hal.hpp"

namespace hal
{
    class Rtc final : public IRtc
    {
      public:
        struct Configuration
        {
            RTC_HandleTypeDef& handle;
        };

        explicit Rtc(Configuration configuration) noexcept;
        ~Rtc() override = default;

        Rtc(const Rtc&) = delete;
        Rtc& operator=(const Rtc&) = delete;
        Rtc(Rtc&&) = delete;
        Rtc& operator=(Rtc&&) = delete;

        [[nodiscard]] auto getTime() const noexcept -> util::Result<Timestamp> override;

      private:
        RTC_HandleTypeDef& m_handle;
    };
}
