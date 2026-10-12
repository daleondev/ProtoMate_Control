#pragma once
#include "../../../step_model/fakes/hal/hal.hpp"

inline TIM_TypeDef encoder_timer, sampler_timer;
inline auto* TIM3 = &encoder_timer;
inline auto* TIM6 = &sampler_timer;
constexpr uint32_t TIM_CHANNEL_ALL = 0x3C, HAL_TIM_STATE_READY = 1;
constexpr uint32_t TIM_FLAG_UPDATE = 1, TIM_FLAG_CC3 = 8, TIM_FLAG_CC4 = 16;
constexpr uint32_t TIM_IT_UPDATE = 1, TIM_IT_CC3 = 8, TIM_IT_CC4 = 16;
struct TIM_HandleTypeDef {
    TIM_TypeDef* Instance{};
    uint32_t State{};
    bool channels_ready{};
};
inline HAL_StatusTypeDef HAL_TIM_Encoder_Stop(TIM_HandleTypeDef* handle, uint32_t)
{
    handle->Instance->CR1 &= ~TIM_CR1_CEN;
    handle->channels_ready = true;
    return HAL_OK;
}
inline HAL_StatusTypeDef HAL_TIM_Encoder_Start(TIM_HandleTypeDef* handle, uint32_t)
{
    if (!handle->channels_ready) { return HAL_ERROR; }
    handle->channels_ready = false;
    handle->Instance->CR1 |= TIM_CR1_CEN;
    return HAL_OK;
}
inline void __HAL_TIM_SET_COUNTER(TIM_HandleTypeDef* handle, uint32_t value) { handle->Instance->CNT = value; }
inline uint32_t __HAL_TIM_GET_COUNTER(TIM_HandleTypeDef* handle) { return handle->Instance->CNT; }
inline void __HAL_TIM_CLEAR_FLAG(TIM_HandleTypeDef* handle, uint32_t flags) { handle->Instance->SR &= ~flags; }
inline void __HAL_TIM_ENABLE_IT(TIM_HandleTypeDef* handle, uint32_t flags) { handle->Instance->DIER |= flags; }
inline void __HAL_TIM_DISABLE_IT(TIM_HandleTypeDef* handle, uint32_t flags) { handle->Instance->DIER &= ~flags; }
