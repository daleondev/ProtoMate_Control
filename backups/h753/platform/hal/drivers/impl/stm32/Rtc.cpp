#include "Rtc.hpp"

#include "hal/stm32/HalResult.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <chrono>
#include <cstdint>

namespace hal
{
    namespace
    {
        constexpr std::int32_t RTC_EPOCH_YEAR{ 2000 };
        constexpr std::int64_t SECONDS_PER_DAY{ 86'400 };
        constexpr std::int64_t SECONDS_PER_HOUR{ 3'600 };
        constexpr std::int64_t SECONDS_PER_MINUTE{ 60 };
        constexpr std::int64_t MINUTES_PER_HOUR{ 60 };
        constexpr std::int64_t HOURS_PER_DAY{ 24 };

        [[nodiscard]] constexpr auto days_from_civil(std::chrono::year_month_day date) noexcept
          -> std::int64_t
        {
            return std::chrono::sys_days{ date }.time_since_epoch().count();
        }

        // Golden calendar fixtures verify the RTC epoch and leap-day conversion.
        // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        static_assert(days_from_civil(std::chrono::year{ 1970 } / 1 / 1) == 0);
        static_assert(days_from_civil(std::chrono::year{ 2000 } / 1 / 1) == 10'957);
        static_assert(days_from_civil(std::chrono::year{ 2000 } / 2 / 29) == 11'016);
        // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

    }

    Rtc::Rtc(Configuration configuration) noexcept
      : m_handle{ configuration.handle }
    {
    }

    auto Rtc::getTime() const noexcept -> util::Result<Timestamp>
    {
        RTC_TimeTypeDef time{};
        RTC_DateTypeDef date{};

        HAL_StatusTypeDef time_status{};
        HAL_StatusTypeDef date_status{};
        {
            // The shadow-register lock is global to the peripheral. Prevent a
            // second thread or interrupt from interleaving another Time/Date
            // read and releasing our snapshot at a calendar rollover.
            const stm32::InterruptGuard interrupt_guard;
            time_status = HAL_RTC_GetTime(&m_handle, &time, RTC_FORMAT_BIN);
            // Always read the date, even if a future HAL implementation can
            // fail GetTime after locking the shadow registers.
            date_status = HAL_RTC_GetDate(&m_handle, &date, RTC_FORMAT_BIN);
        }
        if (time_status != HAL_OK || date_status != HAL_OK) {
            return make_error_result<Timestamp>(HalError::Error);
        }

        const std::int32_t year{ RTC_EPOCH_YEAR + static_cast<std::int32_t>(date.Year) };
        const std::chrono::year_month_day calendar{ std::chrono::year{ year },
                                                    std::chrono::month{ date.Month },
                                                    std::chrono::day{ date.Date } };
        if (!calendar.ok() || time.Hours >= HOURS_PER_DAY || time.Minutes >= MINUTES_PER_HOUR ||
            time.Seconds >= SECONDS_PER_MINUTE || time.SubSeconds > time.SecondFraction) {
            return make_error_result<Timestamp>(HalError::Error);
        }

        const std::int64_t seconds_since_epoch{
            (days_from_civil(calendar) * SECONDS_PER_DAY) +
            (static_cast<std::int64_t>(time.Hours) * SECONDS_PER_HOUR) +
            (static_cast<std::int64_t>(time.Minutes) * SECONDS_PER_MINUTE) + time.Seconds
        };

        const std::uint64_t subsecond_ticks{ time.SecondFraction - time.SubSeconds };
        const std::uint64_t nanoseconds{ subsecond_ticks * NANOSECONDS_PER_SECOND /
                                         (static_cast<std::uint64_t>(time.SecondFraction) + 1U) };

        return Timestamp{ .seconds_since_epoch = seconds_since_epoch,
                          .nanoseconds = static_cast<std::uint32_t>(nanoseconds) };
    }
}
