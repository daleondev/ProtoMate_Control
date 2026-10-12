// CMSIS/HAL macros intentionally use C casts, fixed MMIO addresses and do-while wrappers.
// The line-specific exceptions below preserve the vendor register interface.
#include "hal/drivers/factory/timer.hpp"

#include "hal/drivers/impl/stm32/Timer.hpp"
#include "hal/hal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>

namespace hal::timer
{
    namespace
    {

        [[nodiscard]] auto apb1_timer_input_frequency_hz() noexcept -> std::uint32_t
        {
            RCC_ClkInitTypeDef clock_configuration{};
            std::uint32_t flash_latency{};
            HAL_RCC_GetClockConfig(&clock_configuration, &flash_latency);

            std::uint64_t frequency{ HAL_RCC_GetPCLK1Freq() };
            if (
              (RCC->CFGR & RCC_CFGR_TIMPRE) != // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              0U) { // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                if (clock_configuration.APB1CLKDivider == RCC_HCLK_DIV1 ||
                    clock_configuration.APB1CLKDivider == RCC_HCLK_DIV2 ||
                    clock_configuration.APB1CLKDivider == RCC_HCLK_DIV4) {
                    frequency = HAL_RCC_GetHCLKFreq();
                }
                else {
                    frequency *= 4U;
                }
            }
            else if (clock_configuration.APB1CLKDivider != RCC_HCLK_DIV1) {
                frequency *= 2U;
            }
            return static_cast<std::uint32_t>(
              std::min<std::uint64_t>(frequency, std::numeric_limits<std::uint32_t>::max()));
        }
    }

    auto create(Configuration configuration) -> std::shared_ptr<ITimer>
    {
#if defined(HAL_BOARD_NUCLEO_H755ZIQ) && defined(CORE_CM7)
        if (configuration.peripheral != Peripheral::Tim12) {
            return {};
        }
        static auto timer{ std::make_shared<Timer>(Timer::Configuration{
          .handle = htim12, .input_frequency_hz = apb1_timer_input_frequency_hz(), .interrupt = TIM8_BRK_TIM12_IRQn }) };
#else
        if (configuration.peripheral != Peripheral::Tim5) {
            return {};
        }

        static auto timer{ std::make_shared<Timer>(Timer::Configuration{
          .handle = htim5, .input_frequency_hz = apb1_timer_input_frequency_hz(), .interrupt = TIM5_IRQn }) };
#endif
        return timer;
    }
}
