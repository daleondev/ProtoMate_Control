#include "Rtc.hpp"

#include "hal/drivers/common.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <cstdint>

namespace hal
{
    namespace
    {
        constexpr std::int32_t RTC_EPOCH_YEAR{ 2000 };
        constexpr std::int64_t DAYS_PER_ERA{ 146'097 };
        constexpr std::int64_t DAYS_FROM_CIVIL_EPOCH_OFFSET{ 719'468 };
        constexpr std::int64_t SECONDS_PER_DAY{ 86'400 };
        constexpr std::int64_t SECONDS_PER_HOUR{ 3'600 };
        constexpr std::int64_t SECONDS_PER_MINUTE{ 60 };

        [[nodiscard]] constexpr auto is_leap_year(std::int32_t year) noexcept -> bool
        {
            return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        }

        [[nodiscard]] constexpr auto days_in_month(std::int32_t year, std::uint32_t month) noexcept
          -> std::uint32_t
        {
            switch (month) {
                case 2U:
                    return is_leap_year(year) ? 29U : 28U;
                case 4U:
                case 6U:
                case 9U:
                case 11U:
                    return 30U;
                default:
                    return 31U;
            }
        }

        // Gregorian civil date to days since 1970-01-01.
        [[nodiscard]] constexpr auto days_from_civil(std::int32_t year,
                                                     std::uint32_t month,
                                                     std::uint32_t day) noexcept -> std::int64_t
        {
            year -= month <= 2U ? 1 : 0;
            const std::int32_t era{ (year >= 0 ? year : year - 399) / 400 };
            const auto year_of_era{ static_cast<std::uint32_t>(year - era * 400) };
            const auto adjusted_month{ static_cast<std::int32_t>(month) + (month > 2U ? -3 : 9) };
            const auto day_of_year{ static_cast<std::uint32_t>((153 * adjusted_month + 2) / 5) + day - 1U };
            const std::uint32_t day_of_era{ year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
                                            day_of_year };
            return static_cast<std::int64_t>(era) * DAYS_PER_ERA + day_of_era - DAYS_FROM_CIVIL_EPOCH_OFFSET;
        }

        static_assert(days_from_civil(1970, 1U, 1U) == 0);
        static_assert(days_from_civil(2000, 1U, 1U) == 10'957);
        static_assert(days_from_civil(2000, 2U, 29U) == 11'016);
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
        if (date.Month < 1U || date.Month > 12U || date.Date < 1U ||
            date.Date > days_in_month(year, date.Month) || time.Hours >= 24U || time.Minutes >= 60U ||
            time.Seconds >= 60U || time.SubSeconds > time.SecondFraction) {
            return make_error_result<Timestamp>(HalError::Error);
        }

        const std::int64_t seconds_since_epoch{
            days_from_civil(year, date.Month, date.Date) * SECONDS_PER_DAY +
            static_cast<std::int64_t>(time.Hours) * SECONDS_PER_HOUR +
            static_cast<std::int64_t>(time.Minutes) * SECONDS_PER_MINUTE + time.Seconds
        };

        const std::uint64_t subsecond_ticks{ time.SecondFraction - time.SubSeconds };
        const std::uint64_t nanoseconds{ subsecond_ticks * NANOSECONDS_PER_SECOND /
                                         (static_cast<std::uint64_t>(time.SecondFraction) + 1U) };

        return Timestamp{ .seconds_since_epoch = seconds_since_epoch,
                          .nanoseconds = static_cast<std::uint32_t>(nanoseconds) };
    }
}
