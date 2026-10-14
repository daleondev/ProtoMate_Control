#include "platform.h"

#if !defined(GIGA_APPLICATION) && defined(CORE_CM7)
void giga_peripheral_clocks_initialize(void)
{
    /* RM0399 table 16: VOS0, 240 MHz AXI flash clock requires WRHIGHFREQ=2.
       Cube's SystemClock_Config sets read latency only; the reset value (3)
       is not a documented programming setting for this operating point. */
    MODIFY_REG(FLASH->ACR, FLASH_ACR_WRHIGHFREQ, FLASH_ACR_WRHIGHFREQ_1);
    __DSB();
    /* CubeMX puts independent kernel clocks in MSP callbacks. Establish them
       here before either application; the application wrapper below is read-only. */
    RCC_PeriphCLKInitTypeDef c = {0};
    c.PeriphClockSelection = RCC_PERIPHCLK_LTDC | RCC_PERIPHCLK_USB | RCC_PERIPHCLK_RNG |
        RCC_PERIPHCLK_RTC | RCC_PERIPHCLK_I2C4 | RCC_PERIPHCLK_SPI5 | RCC_PERIPHCLK_USART2;
    c.PLL3.PLL3M = 5;
    c.PLL3.PLL3N = 160;
    c.PLL3.PLL3P = 2;
    c.PLL3.PLL3Q = 2;
    c.PLL3.PLL3R = 10;
    c.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_1;
    c.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
    c.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
    c.RngClockSelection = RCC_RNGCLKSOURCE_HSI48;
    c.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    c.I2c4ClockSelection = RCC_I2C4CLKSOURCE_D3PCLK1;
    c.Spi45ClockSelection = RCC_SPI45CLKSOURCE_D2PCLK1;
    c.Usart234578ClockSelection = RCC_USART234578CLKSOURCE_D2PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&c) != HAL_OK) { giga_fail(0x108); }
}
#endif

#if defined(GIGA_APPLICATION)
HAL_StatusTypeDef __wrap_HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *requested)
{
    const uint64_t selected = requested->PeriphClockSelection;
    const uint64_t allowed = RCC_PERIPHCLK_LTDC | RCC_PERIPHCLK_USB | RCC_PERIPHCLK_RNG |
        RCC_PERIPHCLK_RTC | RCC_PERIPHCLK_I2C4 | RCC_PERIPHCLK_SPI5 | RCC_PERIPHCLK_USART2;
    if (selected & ~allowed) { return HAL_ERROR; }
    RCC_PeriphCLKInitTypeDef current = {0};
    HAL_RCCEx_GetPeriphCLKConfig(&current);
    // HAL's getter normalizes these two masks and omits FRACN; its setter
    // expects register-position masks. Compare the actual register encodings.
    current.PLL3.PLL3RGE = RCC->PLLCFGR & RCC_PLLCFGR_PLL3RGE;
    current.PLL3.PLL3VCOSEL = RCC->PLLCFGR & RCC_PLLCFGR_PLL3VCOSEL;
    current.PLL3.PLL3FRACN = (RCC->PLL3FRACR & RCC_PLL3FRACR_FRACN3) >> RCC_PLL3FRACR_FRACN3_Pos;
#define CHECK(clock, member) if ((selected & (clock)) && current.member != requested->member) { return HAL_ERROR; }
    CHECK(RCC_PERIPHCLK_USB, UsbClockSelection)
    CHECK(RCC_PERIPHCLK_RNG, RngClockSelection)
    CHECK(RCC_PERIPHCLK_RTC, RTCClockSelection)
    CHECK(RCC_PERIPHCLK_I2C4, I2c4ClockSelection)
    CHECK(RCC_PERIPHCLK_SPI5, Spi45ClockSelection)
    CHECK(RCC_PERIPHCLK_USART2, Usart234578ClockSelection)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3M)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3N)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3P)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3Q)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3R)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3RGE)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3VCOSEL)
    CHECK(RCC_PERIPHCLK_LTDC, PLL3.PLL3FRACN)
#undef CHECK
    if ((selected & RCC_PERIPHCLK_LTDC) && !__HAL_RCC_GET_FLAG(RCC_FLAG_PLL3RDY)) { return HAL_ERROR; }
    return HAL_OK;
}
#endif
