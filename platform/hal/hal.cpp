#include "hal.hpp"

#include <cstdint>

namespace hal
{
    auto initialize() noexcept -> void
    {
        constexpr std::uint32_t com_baud_rate{ 115200U };

        MPU_Config_User();
        SCB_EnableICache();
        SCB_EnableDCache();

        if (HAL_Init() != HAL_OK) {
            Error_Handler();
        }

        SystemClock_Config();

        MX_GPIO_Init();
        MX_ETH_Init();
        MX_RTC_Init();
        MX_TIM2_Init();
        MX_TIM3_Init();
        MX_TIM8_Init();
        MX_RNG_Init();

        COM_InitTypeDef bsp_com_init{};
        bsp_com_init.BaudRate = com_baud_rate;
        bsp_com_init.WordLength = COM_WORDLENGTH_8B;
        bsp_com_init.StopBits = COM_STOPBITS_1;
        bsp_com_init.Parity = COM_PARITY_NONE;
        bsp_com_init.HwFlowCtl = COM_HWCONTROL_NONE;

        if (BSP_COM_Init(COM1, &bsp_com_init) != BSP_ERROR_NONE) {
            Error_Handler();
        }
#if defined(HAL_PLATFORM_STM32)
        // Enabling FIFO mode disables the UART briefly. Do this before any
        // application thread can transmit or wait for console input.
        if (HAL_UARTEx_EnableFifoMode(&hcom_uart[COM1]) != HAL_OK) {
            Error_Handler();
        }
#endif
    }

    auto panic(const char* message, std::source_location location) noexcept -> void
    {
        panic(message, nullptr, location);
    }

    auto panic(const char* message, const char* detail, std::source_location location) noexcept -> void
    {
        const HalPanicInfo info{
            .message = message,
            .detail = detail,
            .file = location.file_name(),
            .function = location.function_name(),
            .line = location.line(),
        };
        hal_panic_handler(&info);
    }
}

extern "C" [[noreturn]] void hal_error_handler() noexcept
{
    const HalPanicInfo info{
        .message = "HAL Error_Handler invoked",
        .detail = nullptr,
        .file = nullptr,
        .function = nullptr,
        .line = 0U,
    };
    hal_panic_handler(&info);
}

// The STM32-generated main.h declares this without attributes. Keep this
// definition attribute-free so regenerating that header cannot create a C++
// declaration mismatch. hal_error_handler() still guarantees no return.
extern "C" void Error_Handler() { hal_error_handler(); }
