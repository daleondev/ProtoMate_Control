#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "gpio.h"
#include "main.h"
#include "tim.h"
#if !defined(HAL_BOARD_NUCLEO_H755ZIQ)
#include "eth.h"
#include "dma.h"
#include "rng.h"
#include "rtc.h"
#include "usart.h"
#endif

void SystemClock_Config(void);
void MPU_Config_User(void);

#ifdef __cplusplus
}
#endif
