#include "hal/hal.hpp"
#include "hal/drivers/impl/stm32/Gpio.hpp"

#include <cstdint>

namespace
{
    constexpr std::uint32_t BOOT_SEMAPHORE{0};

#if defined(CORE_CM7)
    void wait_for_m4(bool awake)
    {
        std::uint32_t remaining{1'000'000};
        while ((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) != awake) {
            if (remaining-- == 0) {
                hal::panic("M4 startup timeout");
            }
        }
    }
#endif
}

namespace hal
{
    void initialize() noexcept
    {
#if defined(CORE_CM4)
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
        MX_TIM5_Init();
#else
        wait_for_m4(false);
        if (HAL_Init() != HAL_OK) {
            Error_Handler();
        }
        SystemClock_Config();
        MX_GPIO_Init();
        MX_TIM12_Init();
        // Enable all currently owned clocks before releasing M4. Subsequent
        // factories only re-enable already set bits in the shared RCC registers.
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_TIM5_CLK_ENABLE();
        __HAL_RCC_HSEM_CLK_ENABLE();
        if (HAL_HSEM_FastTake(BOOT_SEMAPHORE) != HAL_OK) {
            Error_Handler();
        }
        HAL_HSEM_Release(BOOT_SEMAPHORE, 0);
        wait_for_m4(true);
        SCB_EnableICache();
        // Private AXI data is cacheable. Shared SRAM/cache policy is deliberately
        // deferred until the shared-memory transport is implemented.
        SCB_EnableDCache();
#endif
        GpioInput::initializeInterrupts();
    }

    [[noreturn]] void panic(const char* message, std::source_location location) noexcept
    {
        panic(message, nullptr, location);
    }

    [[noreturn]] void panic(const char* message, const char* detail, std::source_location location) noexcept
    {
        const HalPanicInfo info{message, detail, location.file_name(), location.function_name(), location.line()};
        hal_panic_handler(&info);
    }
}

// Private to each core and readable in the debugger without console ownership.
extern "C" {
    HalPanicInfo hal_last_panic{};
}

extern "C" [[noreturn]] __attribute__((weak)) void hal_panic_handler(const HalPanicInfo* info) noexcept
{
    __disable_irq();
    if (info) {
        hal_last_panic = *info;
    }
    // Flush diagnostics for SWD on M7 before parking, without touching peer GPIOs.
#if defined(CORE_CM7)
    SCB_CleanDCache();
#endif
    for (;;) {
        __NOP();
    }
}

extern "C" [[noreturn]] void hal_error_handler() noexcept { hal::panic("HAL initialization failed"); }
extern "C" [[noreturn]] void hal_fault_handler(const char* fault) noexcept { hal::panic(fault); }
extern "C" void Error_Handler() { hal_error_handler(); }
extern "C" void HardFault_Handler() { hal_fault_handler("HardFault"); }
extern "C" void MemManage_Handler() { hal_fault_handler("MemManage"); }
extern "C" void BusFault_Handler() { hal_fault_handler("BusFault"); }
extern "C" void UsageFault_Handler() { hal_fault_handler("UsageFault"); }

// No UART/stdio console is assigned in this profile yet. Report unavailable.
extern "C" int __io_putchar(int) { return -1; }
extern "C" int __io_getchar() { return -1; }
