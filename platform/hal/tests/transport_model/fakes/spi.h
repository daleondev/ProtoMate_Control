#pragma once
#include "hal/hal.hpp"
inline SPI_HandleTypeDef hspi5;
inline constexpr unsigned RCC_PERIPHCLK_SPI5 = 5;
inline auto HAL_RCCEx_GetPeriphCLKFreq(unsigned) -> std::uint32_t { return 120'000'000U; }
inline void MX_SPI5_Init()
{
    ++fake::spi_init;
    fake::spi_handle = &hspi5;
    hspi5.Init.BaudRatePrescaler = 6U << SPI_CFG1_MBR_Pos;
}
#define __HAL_RCC_SPI5_FORCE_RESET() (++fake::spi_reset)
#define __HAL_RCC_SPI5_RELEASE_RESET() ((void)0)

inline SPI_HandleTypeDef hspi1;
inline constexpr unsigned RCC_PERIPHCLK_SPI1 = 1;
inline void MX_SPI1_Init()
{
    ++fake::spi_init;
    fake::spi_handle = &hspi1;
    hspi1.Init.BaudRatePrescaler = 2U << SPI_CFG1_MBR_Pos;
}
#define __HAL_RCC_SPI1_FORCE_RESET() (++fake::spi_reset)
#define __HAL_RCC_SPI1_RELEASE_RESET() ((void)0)
