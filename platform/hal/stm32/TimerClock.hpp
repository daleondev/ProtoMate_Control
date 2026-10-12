#pragma once

#include "hal/hal.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace hal::stm32
{
    [[nodiscard]] inline auto apb1_timer_input_frequency_hz() noexcept -> std::uint32_t
    {
        RCC_ClkInitTypeDef clock_configuration{};
        std::uint32_t flash_latency{};
        HAL_RCC_GetClockConfig(&clock_configuration, &flash_latency);

        std::uint64_t frequency{ HAL_RCC_GetPCLK1Freq() };
        if (
          (RCC->CFGR & RCC_CFGR_TIMPRE) != // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          0U) { // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            if (clock_configuration.APB1CLKDivider == RCC_APB1_DIV1 ||
                clock_configuration.APB1CLKDivider == RCC_APB1_DIV2 ||
                clock_configuration.APB1CLKDivider == RCC_APB1_DIV4) {
                frequency = HAL_RCC_GetHCLKFreq();
            }
            else {
                frequency *= 4U;
            }
        }
        else if (clock_configuration.APB1CLKDivider != RCC_APB1_DIV1) {
            frequency *= 2U;
        }
        return static_cast<std::uint32_t>(
          std::min<std::uint64_t>(frequency, std::numeric_limits<std::uint32_t>::max()));
    }
}
