#include "hal/hal.hpp"
#include "hal/drivers/impl/stm32/Gpio.hpp"
#include "hal/drivers/impl/stm32/InterCoreChannel.hpp"
#include "hal/stm32/HardwareSemaphore.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/stm32/FaultShutdown.hpp"

#include <cstdint>
#include "usart.h"
#if defined(CORE_CM4)
#include "dma.h"
#endif
#if defined(CORE_CM7)
#include "rtc.h"
#include "rng.h"
#endif

namespace
{
    constexpr std::uint32_t BOOT_SEMAPHORE{0};

    void initialize_gpio()
    {
        const hal::stm32::InterruptGuard local;
        const hal::stm32::HardwareSemaphore shared(5);
        if (!shared) { hal::panic("GPIO startup lock timeout"); }
        MX_GPIO_Init();
#if defined(CORE_CM4)
        // MSP initialization also writes shared GPIO/RCC registers.
        MX_DMA_Init();
        MX_TIM2_Init();
        MX_TIM3_Init();
        MX_TIM5_Init();
        MX_TIM6_Init();
        MX_TIM7_Init();
        MX_USART2_UART_Init();
        // CubeMX enables these NVIC vectors with DMA initialization. The STEP
        // backend enables them again only after installing its own owner.
        for (const auto irq : {DMA1_Stream0_IRQn, DMA1_Stream1_IRQn,
                               DMA1_Stream2_IRQn, DMA1_Stream3_IRQn}) {
            HAL_NVIC_DisableIRQ(irq);
            HAL_NVIC_ClearPendingIRQ(irq);
        }
#endif
    }

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
        initialize_gpio();
#else
        wait_for_m4(false);
        if (HAL_Init() != HAL_OK) {
            Error_Handler();
        }
        SystemClock_Config();
        __HAL_RCC_HSEM_CLK_ENABLE();
        initialize_gpio();
        MX_TIM12_Init();
        MX_RTC_Init();
        MX_RNG_Init();
        MX_USART3_UART_Init();
        // Enable all currently owned clocks before releasing M4. Subsequent
        // factories only re-enable already set bits in the shared RCC registers.
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();
        __HAL_RCC_GPIOE_CLK_ENABLE();
        __HAL_RCC_TIM2_CLK_ENABLE();
        __HAL_RCC_TIM3_CLK_ENABLE();
        __HAL_RCC_TIM5_CLK_ENABLE();
        __HAL_RCC_TIM6_CLK_ENABLE();
        __HAL_RCC_TIM7_CLK_ENABLE();
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_DMA1_CLK_ENABLE();
        __HAL_RCC_D2SRAM1_CLK_ENABLE();
        __HAL_RCC_HSEM_CLK_ENABLE();
        // Normal non-cacheable, shareable SRAM4. Keep private AXI RAM cached.
        MPU_Region_InitTypeDef shared{};
        shared.Enable = MPU_REGION_ENABLE;
        shared.Number = MPU_REGION_NUMBER0;
        shared.BaseAddress = 0x38000000;
        shared.Size = MPU_REGION_SIZE_64KB;
        shared.TypeExtField = MPU_TEX_LEVEL1;
        shared.AccessPermission = MPU_REGION_FULL_ACCESS;
        shared.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
        shared.IsShareable = MPU_ACCESS_SHAREABLE;
        shared.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
        shared.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
        HAL_MPU_Disable();
        HAL_MPU_ConfigRegion(&shared);
        HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
        InterCoreChannel::initializeMemory();
        if (HAL_HSEM_FastTake(BOOT_SEMAPHORE) != HAL_OK) {
            Error_Handler();
        }
        HAL_HSEM_Release(BOOT_SEMAPHORE, 0);
        wait_for_m4(true);
        SCB_EnableICache();
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
#if defined(CORE_CM4)
    hal::stm32::shutdown_motion_on_fault();
#endif
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

// USART3/ST-Link VCP belongs to M7. M4 must send future log messages through ICC.
extern "C" int __io_putchar(int value)
{
#if defined(CORE_CM7)
    const auto byte = static_cast<std::uint8_t>(value);
    return HAL_UART_Transmit(&huart3, &byte, 1, 100) == HAL_OK ? value : -1;
#else
    static_cast<void>(value);
    return -1;
#endif
}
extern "C" int __io_getchar()
{
#if defined(CORE_CM7)
    std::uint8_t byte{};
    return HAL_UART_Receive(&huart3, &byte, 1, HAL_MAX_DELAY) == HAL_OK ? byte : -1;
#else
    return -1;
#endif
}
