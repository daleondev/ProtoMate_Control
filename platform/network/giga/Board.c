#include "cybsp.h"
#include "stm32_cyhal_sdio_ex.h"
#include "tx_api.h"
#include <stdio.h>
#include <string.h>

static SD_HandleTypeDef sdio;
static char nvram[2048];
extern uint32_t dynamic_nvram_size;
extern void *dynamic_nvram_image;
void _tx_execution_isr_enter(void);
void _tx_execution_isr_exit(void);

int wifi_board_initialize(void) {
    if(dynamic_nvram_size > sizeof(nvram)) return -2;
    memcpy(nvram, dynamic_nvram_image, dynamic_nvram_size);
    const uint32_t a = HAL_GetUIDw0() ^ HAL_GetUIDw2();
    const uint32_t b = HAL_GetUIDw1();
    int mac_found = 0;
    for(uint32_t i = 0; i + 25 <= dynamic_nvram_size; ++i) {
        if(memcmp(nvram + i, "macaddr=", 8) != 0) continue;
        char address[26];
        snprintf(address, sizeof(address), "macaddr=02:%02x:%02x:%02x:%02x:%02x",
            (unsigned)(a & 255), (unsigned)((a >> 8) & 255), (unsigned)((a >> 16) & 255),
            (unsigned)(b & 255), (unsigned)((b >> 8) & 255));
        memcpy(nvram + i, address, 25);
        mac_found = 1;
        break;
    }
    if(!mac_found) return -3;
    dynamic_nvram_image = nvram;
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SDMMC1_CLK_ENABLE();
    /* Existing PLL1 Q is 48 MHz. No PLL or shared clock-tree reconfiguration. */
    if(__HAL_RCC_GET_SDMMC_SOURCE() != RCC_SDMMCCLKSOURCE_PLL ||
       HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SDMMC) != 48000000U) return -1;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_10;
    pin.Mode = GPIO_MODE_OUTPUT_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &pin);
    sdio.Instance = SDMMC1;
    return (int)stm32_cypal_sdio_hw_init(&sdio);
}

void HAL_SD_MspInit(SD_HandleTypeDef *handle) {
    if(handle->Instance != SDMMC1) return;
    GPIO_InitTypeDef pin = {0};
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_PULLUP;
    pin.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    pin.Alternate = GPIO_AF12_SDMMC1;
    pin.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
    HAL_GPIO_Init(GPIOC, &pin);
    pin.Pin = GPIO_PIN_12;
    pin.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &pin);
    pin.Pin = GPIO_PIN_2;
    pin.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOD, &pin);
}

void SDMMC1_IRQHandler(void) {
    _tx_execution_isr_enter();
    stm32_cyhal_sdio_irq_handler();
    _tx_execution_isr_exit();
}
