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
#define M1_ENC_A_Pin GPIO_PIN_4
#define M1_ENC_A_GPIO_Port GPIOB
#define M2_REFERENCE_Pin GPIO_PIN_5
#define M2_REFERENCE_GPIO_Port GPIOE
#define M1_REFERENCE_Pin GPIO_PIN_4
#define M1_REFERENCE_GPIO_Port GPIOE
#define M3_INDEX_Pin GPIO_PIN_9
#define M3_INDEX_GPIO_Port GPIOB
#define M2_INDEX_Pin GPIO_PIN_8
#define M2_INDEX_GPIO_Port GPIOB
#define M3_DIR_Pin GPIO_PIN_13
#define M3_DIR_GPIO_Port GPIOH
#define STEPPERS_ENABLE_Pin GPIO_PIN_7
#define STEPPERS_ENABLE_GPIO_Port GPIOC
#define M1_ENC_Z_Pin GPIO_PIN_7
#define M1_ENC_Z_GPIO_Port GPIOG
#define ESC_CS_Pin GPIO_PIN_1
#define ESC_CS_GPIO_Port GPIOK
#define M3_DIAG_Pin GPIO_PIN_0
#define M3_DIAG_GPIO_Port GPIOC
#define M2_DIAG_Pin GPIO_PIN_3
#define M2_DIAG_GPIO_Port GPIOC
#define M2_STEP_Pin GPIO_PIN_2
#define M2_STEP_GPIO_Port GPIOA
#define M1_STEP_Pin GPIO_PIN_0
#define M1_STEP_GPIO_Port GPIOA
#define M1_DIR_Pin GPIO_PIN_8
#define M1_DIR_GPIO_Port GPIOJ
#define ESC_IRQ_Pin GPIO_PIN_10
#define ESC_IRQ_GPIO_Port GPIOH
#define ESC_SYNC0_Pin GPIO_PIN_11
#define ESC_SYNC0_GPIO_Port GPIOH
#define M1_ENC_B_Pin GPIO_PIN_7
#define M1_ENC_B_GPIO_Port GPIOA
#define M3_REFERENCE_Pin GPIO_PIN_2
#define M3_REFERENCE_GPIO_Port GPIOB
#define ESC_RESET_Pin GPIO_PIN_9
#define ESC_RESET_GPIO_Port GPIOH
#define M2_DIR_Pin GPIO_PIN_13
#define M2_DIR_GPIO_Port GPIOD
#define M1_ALARM_Pin GPIO_PIN_1
#define M1_ALARM_GPIO_Port GPIOB
#define M3_STEP_Pin GPIO_PIN_3
#define M3_STEP_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
