/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

#include "stm32h7xx_nucleo.h"
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define M1_STEP_Pin GPIO_PIN_0
#define M1_STEP_GPIO_Port GPIOA
#define M1_REF_Pin GPIO_PIN_7
#define M1_REF_GPIO_Port GPIOE
#define M1_REF_EXTI_IRQn EXTI9_5_IRQn
#define M2_REF_Pin GPIO_PIN_8
#define M2_REF_GPIO_Port GPIOE
#define M2_REF_EXTI_IRQn EXTI9_5_IRQn
#define M3_REF_Pin GPIO_PIN_10
#define M3_REF_GPIO_Port GPIOE
#define M3_REF_EXTI_IRQn EXTI15_10_IRQn
#define M1_DIR_Pin GPIO_PIN_12
#define M1_DIR_GPIO_Port GPIOE
#define M2_DIR_Pin GPIO_PIN_13
#define M2_DIR_GPIO_Port GPIOE
#define M3_DIR_Pin GPIO_PIN_14
#define M3_DIR_GPIO_Port GPIOE
#define STEPPERS_EN_N_Pin GPIO_PIN_15
#define STEPPERS_EN_N_GPIO_Port GPIOE
#define M2_STEP_Pin GPIO_PIN_10
#define M2_STEP_GPIO_Port GPIOB
#define M3_STEP_Pin GPIO_PIN_11
#define M3_STEP_GPIO_Port GPIOB
#define SD_CARD_DETECT_Pin GPIO_PIN_2
#define SD_CARD_DETECT_GPIO_Port GPIOG
#define M3_DIAG_Pin GPIO_PIN_3
#define M3_DIAG_GPIO_Port GPIOD
#define M3_DIAG_EXTI_IRQn EXTI3_IRQn
#define M2_DIAG_Pin GPIO_PIN_4
#define M2_DIAG_GPIO_Port GPIOD
#define M2_DIAG_EXTI_IRQn EXTI4_IRQn
#define TMC_UART_TX_Pin GPIO_PIN_5
#define TMC_UART_TX_GPIO_Port GPIOD
#define TMC_UART_RX_Pin GPIO_PIN_6
#define TMC_UART_RX_GPIO_Port GPIOD
#define M1_ENC_A_Pin GPIO_PIN_4
#define M1_ENC_A_GPIO_Port GPIOB
#define M1_ENC_B_Pin GPIO_PIN_5
#define M1_ENC_B_GPIO_Port GPIOB
#define M1_ENC_Z_Pin GPIO_PIN_6
#define M1_ENC_Z_GPIO_Port GPIOB
#define M1_ENC_Z_EXTI_IRQn EXTI9_5_IRQn

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
