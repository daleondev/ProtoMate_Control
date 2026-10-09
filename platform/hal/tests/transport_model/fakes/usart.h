#pragma once
#include "hal/hal.hpp"
inline USART_TypeDef usart2;
inline UART_HandleTypeDef huart2{ &usart2 };
inline void MX_USART2_UART_Init()
{
    ++fake::uart_init;
    huart2.FifoMode = UART_FIFOMODE_ENABLE;
}

inline USART_TypeDef usart1;
inline UART_HandleTypeDef huart1{ &usart1 };
inline void MX_USART1_UART_Init()
{
    ++fake::uart_init;
    huart1.FifoMode = UART_FIFOMODE_ENABLE;
}
