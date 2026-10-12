#include "main.h"
#include "gpio.h"

#include <cstdint>

namespace
{
    constexpr std::uint32_t BOOT_SEMAPHORE{0};
    constexpr std::uint32_t LED_HALF_PERIOD_MS{250};
}

int main()
{
    // ST's dual-core boot sequence: park M4 while M7 configures shared clocks.
    // This is startup synchronization only; the applications exchange no data.
    __HAL_RCC_HSEM_CLK_ENABLE();
    HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(BOOT_SEMAPHORE));
    HAL_PWREx_ClearPendingEvent();
    HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(BOOT_SEMAPHORE));
    HAL_HSEM_DeactivateNotification(__HAL_HSEM_SEMID_TO_MASK(BOOT_SEMAPHORE));

    SystemCoreClockUpdate();
    if (HAL_Init() != HAL_OK) {
        Error_Handler();
    }
    MX_GPIO_Init();

    // LD1 / PB0: green, 2 Hz. M4 owns this GPIO port's configuration.
    while (true) {
        HAL_GPIO_TogglePin(IO_LED_GPIO_Port, IO_LED_Pin);
        HAL_Delay(LED_HALF_PERIOD_MS);
    }
}
