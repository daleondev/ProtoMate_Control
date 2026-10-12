#include "main.h"
#include "gpio.h"

#include <cstdint>

extern "C" void SystemClock_Config();

namespace
{
    constexpr std::uint32_t BOOT_SEMAPHORE{0};
    constexpr std::uint32_t BOOT_WAIT_ITERATIONS{1'000'000};
    constexpr std::uint32_t LED_HALF_PERIOD_MS{500};

    // Used before HAL/SysTick initialization, so a bounded register wait is
    // intentional. Both firmware images must be flashed before a board reset.
    void wait_for_m4(bool awake)
    {
        auto remaining = BOOT_WAIT_ITERATIONS;
        while ((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) != awake) {
            if (remaining-- == 0) {
                Error_Handler();
            }
        }
    }
}

int main()
{
    wait_for_m4(false);
    if (HAL_Init() != HAL_OK) {
        Error_Handler();
    }
    // CubeMX selects direct SMPS and the crystal-derived 5 MHz ST-Link HSE input.
    SystemClock_Config();
    MX_GPIO_Init();

    // All M7 clock/GPIO setup completes before M4 may configure its own GPIOs.
    __HAL_RCC_HSEM_CLK_ENABLE();
    if (HAL_HSEM_FastTake(BOOT_SEMAPHORE) != HAL_OK) {
        Error_Handler();
    }
    HAL_HSEM_Release(BOOT_SEMAPHORE, 0);
    wait_for_m4(true);

    // LD2 / PE1: yellow, 1 Hz. No RTOS, robot code, or p2p transport is started.
    while (true) {
        HAL_GPIO_TogglePin(UI_LED_GPIO_Port, UI_LED_Pin);
        HAL_Delay(LED_HALF_PERIOD_MS);
    }
}
