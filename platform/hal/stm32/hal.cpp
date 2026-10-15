#include "hal/hal.hpp"
#include "hal/console.hpp"
#include "hal/drivers/impl/stm32/Gpio.hpp"
#include "hal/drivers/impl/stm32/InterCoreChannel.hpp"
#include "hal/stm32/HardwareSemaphore.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/stm32/FaultShutdown.hpp"

#include <cstdint>
#include <array>
#include <atomic>
#if defined(CORE_CM4)
#include "usart.h"
#include "dma.h"
#include "spi.h"
#endif
#if defined(CORE_CM7)
#include "rtc.h"
#include "rng.h"
#endif

#include "platform.h"
#if defined(CORE_CM7)
#include "usb_device.h"
#include "ltdc.h"
#include "dma2d.h"
#include "crc.h"
#include "i2c.h"
#endif

namespace hal
{
    void initialize() noexcept
    {
        // Shared clocks, QSPI, FMC, MPU and caches belong to the bootstrap.
        SystemCoreClockUpdate();
        if (HAL_Init() != HAL_OK) { Error_Handler(); }
        {
            // Startup runs before application threads/callback registration.
            // Leave SysTick enabled so HAL peripheral timeout loops stay bounded.
            const stm32::HardwareSemaphore shared(5);
            if (!shared) { panic("GPIO startup lock timeout"); }
            MX_GPIO_Init();
#if defined(CORE_CM4)
            MX_DMA_Init();
            MX_TIM2_Init();
            MX_TIM3_Init();
            MX_TIM5_Init();
            MX_TIM6_Init();
            MX_TIM7_Init();
            MX_USART2_UART_Init();
            MX_SPI5_Init();
            // Owners install callbacks before enabling STEP DMA interrupts.
            for (const auto irq : {DMA1_Stream0_IRQn, DMA1_Stream1_IRQn,
                                   DMA1_Stream2_IRQn, DMA1_Stream3_IRQn}) {
                HAL_NVIC_DisableIRQ(irq);
                HAL_NVIC_ClearPendingIRQ(irq);
            }
#else
            MX_TIM12_Init();
            MX_RTC_Init();
            MX_RNG_Init();
            MX_TIM8_Init();
            MX_CRC_Init();
            MX_DMA2D_Init();
            MX_LTDC_Init();
            __HAL_LTDC_DISABLE(&hltdc);
            MX_I2C4_Init();
#endif
        }
#if defined(CORE_CM7)
        InterCoreChannel::initializeMemory();
        HAL_PWREx_EnableUSBVoltageDetector();
        MX_USB_DEVICE_Init();
#endif
        GpioInput::initializeInterrupts();
    }

    void start_secondary_cores() noexcept
    {
#if defined(CORE_CM7)
        giga_release_m4();
#endif
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

