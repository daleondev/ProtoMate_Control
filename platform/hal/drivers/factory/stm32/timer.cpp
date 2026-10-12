// CMSIS/HAL macros intentionally use C casts, fixed MMIO addresses and do-while wrappers.
// The line-specific exceptions below preserve the vendor register interface.
#include "hal/drivers/factory/timer.hpp"

#include "hal/drivers/impl/stm32/Timer.hpp"
#include "hal/stm32/TimerClock.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>

namespace hal::timer
{
    auto create(Configuration configuration) -> std::shared_ptr<ITimer>
    {
        // CubeMX startup owns hardware initialization. Each process keeps its
        // own handle, selected by peripheral rather than by executing core.
        // Constructing a wrapper neither resets nor reconfigures the timer.
        switch (configuration.peripheral) {
            case Peripheral::Tim5: {
                static TIM_HandleTypeDef handle{};
                static auto instance = [] {
                    handle.Instance = TIM5;
                    handle.State = HAL_TIM_STATE_READY;
                    return std::make_shared<Timer>(Timer::Configuration{
                        .handle = handle, .input_frequency_hz = stm32::apb1_timer_input_frequency_hz(),
                        .interrupt = TIM5_IRQn});
                }();
                return instance;
            }
            case Peripheral::Tim12: {
                static TIM_HandleTypeDef handle{};
                static auto instance = [] {
                    handle.Instance = TIM12;
                    handle.State = HAL_TIM_STATE_READY;
                    return std::make_shared<Timer>(Timer::Configuration{
                        .handle = handle, .input_frequency_hz = stm32::apb1_timer_input_frequency_hz(),
                        .interrupt = TIM8_BRK_TIM12_IRQn});
                }();
                return instance;
            }
            default: return {};
        }
    }
}
