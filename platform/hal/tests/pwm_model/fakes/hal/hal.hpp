#pragma once

// Minimal behavioral timer model for compiling the REAL STM32 PWM driver on a
// host. Models PWM1/2, preload, OPM, sticky write-zero-to-clear flags and NVIC.
// It is not a substitute for electrical/timing validation on the target.
#include <array>
#include <cstdint>

using std::uint32_t;
enum HAL_StatusTypeDef
{
    HAL_OK,
    HAL_ERROR,
    HAL_BUSY,
    HAL_TIMEOUT
};
enum IRQn_Type
{
    TIM1_UP_IRQn,
    TIM1_CC_IRQn,
    TIM4_IRQn,
    TIM8_UP_TIM13_IRQn,
    TIM8_CC_IRQn
};
inline std::array<bool, 5U> irq_enabled{};
inline uint32_t primask{};
inline uint32_t __get_PRIMASK() { return primask; }
inline void __disable_irq() { primask = 1U; }
inline void __set_PRIMASK(uint32_t value) { primask = value; }
inline void __DMB() {}
inline void HAL_NVIC_DisableIRQ(IRQn_Type irq) { irq_enabled[irq] = false; }
inline void HAL_NVIC_EnableIRQ(IRQn_Type irq) { irq_enabled[irq] = true; }
inline void HAL_NVIC_ClearPendingIRQ(IRQn_Type) {}
inline void HAL_NVIC_SetPriority(IRQn_Type, uint32_t, uint32_t) {}

constexpr uint32_t TIM_CR1_CEN = 1U, TIM_CR1_OPM = 8U, TIM_CR1_DIR = 16U, TIM_CR1_CMS = 96U;
constexpr uint32_t TIM_CR1_URS = 4U, TIM_CR1_UDIS = 2U;
constexpr uint32_t TIM_FLAG_UPDATE = 1U, TIM_FLAG_CC1 = 2U, TIM_FLAG_CC3 = 8U;
constexpr uint32_t TIM_IT_UPDATE = TIM_FLAG_UPDATE, TIM_EGR_UG = 1U;
constexpr uint32_t TIM_CHANNEL_1 = 0U, TIM_CHANNEL_3 = 8U;
constexpr uint32_t TIM_CCx_ENABLE = 1U;
constexpr uint32_t TIM_OCMODE_PWM1 = 6U, TIM_OCMODE_PWM2 = 7U;
constexpr uint32_t TIM_OCPOLARITY_HIGH = 0U, TIM_OCNPOLARITY_HIGH = 0U;
constexpr uint32_t TIM_OCFAST_DISABLE = 0U, TIM_OCIDLESTATE_RESET = 0U, TIM_OCNIDLESTATE_RESET = 0U;
#define SET_BIT(reg, bits) ((reg) = (reg) | (bits))
#define CLEAR_BIT(reg, bits) ((reg) = (reg) & ~(bits))
#define MODIFY_REG(reg, clear, set) ((reg) = ((reg) & ~(clear)) | (set))

struct StatusRegister
{
    uint32_t value{};
    operator uint32_t() const { return value; }
    void operator=(uint32_t written) { value &= written; }
    void raise(uint32_t bits) { value |= bits; }
};
struct TIM_TypeDef
{
    uint32_t CR1{}, SMCR{}, DIER{}, RCR{}, CNT{}, PSC{}, ARR{}, CCR{}, active_compare{};
    uint32_t mode{}, channel{}, af{}, pin{};
    bool enabled{};
    StatusRegister SR;
    struct EventRegister
    {
        TIM_TypeDef* timer;
        void operator=(uint32_t) { timer->update(); }
    } EGR{ this };

    void update()
    {
        CNT = 0U;
        active_compare = CCR;
        SR.raise(TIM_FLAG_UPDATE);
        if ((CR1 & TIM_CR1_OPM) != 0U) {
            CR1 &= ~TIM_CR1_CEN;
        }
    }
    void tick()
    {
        if ((CR1 & TIM_CR1_CEN) == 0U) {
            return;
        }
        if (CNT == ARR) {
            update();
        }
        else {
            ++CNT;
        }
        if (CNT == active_compare) {
            SR.raise(channel == TIM_CHANNEL_1 ? TIM_FLAG_CC1 : TIM_FLAG_CC3);
        }
    }
    bool high() const
    {
        return enabled && (mode == TIM_OCMODE_PWM2 ? CNT >= active_compare : CNT < active_compare);
    }
};
struct TIM_HandleTypeDef
{
    TIM_TypeDef* Instance;
};
inline TIM_TypeDef timer1, timer4, timer8;
inline TIM_HandleTypeDef htim1{ &timer1 }, htim4{ &timer4 }, htim8{ &timer8 };
#define __HAL_TIM_SET_PRESCALER(h, value) ((h)->Instance->PSC = (value))
#define __HAL_TIM_SET_AUTORELOAD(h, value) ((h)->Instance->ARR = (value))
#define __HAL_TIM_SET_COMPARE(h, ch, value) ((h)->Instance->CCR = (value))
#define __HAL_TIM_ENABLE_OCxPRELOAD(h, ch) ((void)0)
#define __HAL_TIM_CLEAR_FLAG(h, flag) ((h)->Instance->SR = ~(flag))
#define __HAL_TIM_MOE_ENABLE(h) ((void)0)

inline void TIM_CCxChannelCmd(TIM_TypeDef* timer, uint32_t, uint32_t enabled)
{
    timer->enabled = enabled != 0U;
}

struct TIM_OC_InitTypeDef
{
    uint32_t OCMode{}, Pulse{}, OCPolarity{}, OCNPolarity{}, OCFastMode{}, OCIdleState{}, OCNIdleState{};
};
inline HAL_StatusTypeDef next_start_status{ HAL_OK };
inline HAL_StatusTypeDef HAL_TIM_PWM_ConfigChannel(TIM_HandleTypeDef* handle,
                                                   const TIM_OC_InitTypeDef* config,
                                                   uint32_t channel)
{
    handle->Instance->mode = config->OCMode;
    handle->Instance->CCR = config->Pulse;
    handle->Instance->channel = channel;
    return HAL_OK;
}
inline HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* handle, uint32_t)
{
    const auto status = next_start_status;
    next_start_status = HAL_OK;
    if (status == HAL_OK) {
        handle->Instance->enabled = true;
        handle->Instance->CR1 |= TIM_CR1_CEN;
    }
    return status;
}
inline HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef* handle, uint32_t)
{
    handle->Instance->enabled = false;
    handle->Instance->CR1 &= ~TIM_CR1_CEN;
    return HAL_OK;
}

constexpr uint32_t GPIO_PIN_RESET = 0U, GPIO_MODE_AF_PP = 2U, GPIO_MODE_OUTPUT_PP = 1U;
constexpr uint32_t GPIO_NOPULL = 0U, GPIO_SPEED_FREQ_LOW = 0U;
struct GPIO_TypeDef
{
    uint32_t mode{}, latch{};
};
inline GPIO_TypeDef gpio_e, gpio_d, gpio_c;
inline GPIO_TypeDef* const GPIOE = &gpio_e;
inline GPIO_TypeDef* const GPIOD = &gpio_d;
inline GPIO_TypeDef* const GPIOC = &gpio_c;
struct GPIO_InitTypeDef
{
    uint32_t Pin{}, Mode{}, Pull{}, Speed{}, Alternate{};
};
inline void HAL_GPIO_WritePin(GPIO_TypeDef* port, uint32_t, uint32_t level) { port->latch = level; }
inline void HAL_GPIO_Init(GPIO_TypeDef* port, const GPIO_InitTypeDef* config) { port->mode = config->Mode; }

constexpr uint32_t RCC_CFGR_TIMPRE = 1U, RCC_HCLK_DIV1 = 1U, RCC_HCLK_DIV2 = 2U, RCC_HCLK_DIV4 = 4U;
struct RCC_TypeDef
{
    uint32_t CFGR{};
};
inline RCC_TypeDef rcc;
inline RCC_TypeDef* const RCC = &rcc;
struct RCC_ClkInitTypeDef
{
    uint32_t APB1CLKDivider{}, APB2CLKDivider{};
};
inline void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef* clocks, uint32_t*)
{
    clocks->APB1CLKDivider = RCC_HCLK_DIV2;
    clocks->APB2CLKDivider = RCC_HCLK_DIV2;
}
inline uint32_t HAL_RCC_GetPCLK1Freq() { return 120'000'000U; }
inline uint32_t HAL_RCC_GetPCLK2Freq() { return 120'000'000U; }
inline uint32_t HAL_RCC_GetHCLKFreq() { return 240'000'000U; }
