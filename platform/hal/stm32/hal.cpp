#include "hal/hal.hpp"
#include "hal/console.hpp"
#include "hal/drivers/impl/stm32/Gpio.hpp"
#include "hal/drivers/impl/stm32/InterCoreChannel.hpp"
#include "hal/stm32/FaultShutdown.hpp"
#include "hal/stm32/HardwareSemaphore.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "profiling/Trace.h"

#include <array>
#include <atomic>
#include <cstdint>
#if defined(CORE_CM4)
#include "dma.h"
#include "spi.h"
#include "usart.h"
#endif
#if defined(CORE_CM7)
#include "rng.h"
#include "rtc.h"
#endif

#include "platform.h"
#if defined(CORE_CM7)
#include "crc.h"
#include "dma2d.h"
#include "i2c.h"
#include "ltdc.h"
#include "usb_device.h"
#endif

namespace hal
{
    void initialize() noexcept
    {
        // Shared clocks, QSPI, FMC, MPU and caches belong to the bootstrap.
        SystemCoreClockUpdate();
        if (HAL_Init() != HAL_OK) {
            Error_Handler();
        }
        {
            // Startup runs before application threads/callback registration.
            // Leave SysTick enabled so HAL peripheral timeout loops stay bounded.
            const stm32::HardwareSemaphore shared{ 5 };
            if (!shared) {
                panic("GPIO startup lock timeout");
            }
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
            for (const auto irq :
                 { DMA1_Stream0_IRQn, DMA1_Stream1_IRQn, DMA1_Stream2_IRQn, DMA1_Stream3_IRQn }) {
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

    auto reset_system() noexcept -> util::Result<>
    {
        stm32::shutdown_motion_on_fault();
        // SYSRESETREQ resets the entire H747. CPU-only WWDG reset is explicitly
        // not guaranteed in RM0399 9.7.36; never use it for individual restarts.
        NVIC_SystemReset();
    }

    [[noreturn]] void panic(const char* message, std::source_location location) noexcept
    {
        panic(message, nullptr, location);
    }

    [[noreturn]] void panic(const char* message, const char* detail, std::source_location location) noexcept
    {
        const HalPanicInfo info{
            message, detail, location.file_name(), location.function_name(), location.line()
        };
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
extern "C" [[noreturn]] void hal_fault_handler(const char* fault) noexcept
{
    hal::stm32::shutdown_motion_on_fault();
    profile_failure(PROFILE_FATAL,
                    __get_IPSR(),
                    0,
                    // Record the return address as diagnostic data, never dereference it.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    hal::panic(fault);
}
extern "C" void Error_Handler() { hal_error_handler(); }
extern "C" void HardFault_Handler() { hal_fault_handler("HardFault"); }
extern "C" void MemManage_Handler() { hal_fault_handler("MemManage"); }
extern "C" void BusFault_Handler() { hal_fault_handler("BusFault"); }
extern "C" void UsageFault_Handler() { hal_fault_handler("UsageFault"); }
