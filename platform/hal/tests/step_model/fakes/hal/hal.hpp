#pragma once

// Register boundary for compiling the real STM32 step adapter on the host.
#include <array>
#include <cstddef>
#include <cstdint>
using std::uint32_t;
#define SET_BIT(reg, bits) ((reg) = (reg) | (bits))
#define CLEAR_BIT(reg, bits) ((reg) = (reg) & ~(bits))
constexpr uint32_t TIM_CR1_CEN = 1, TIM_EGR_UG = 1, TIM_SR_UIF = 1;
constexpr uint32_t TIM_SR_CC1IF = 2, TIM_SR_CC3IF = 8, TIM_SR_CC4IF = 16;
constexpr uint32_t TIM_DIER_CC1DE = 1U << 9, TIM_DIER_CC2DE = 1U << 10, TIM_DIER_CC3DE = 1U << 11,
                   TIM_DIER_CC4DE = 1U << 12;
constexpr uint32_t TIM_CCER_CC1E = 1, TIM_CCER_CC3E = 1U << 8, TIM_CCER_CC4E = 1U << 12;
constexpr uint32_t TIM_OCMODE_TOGGLE = 3U << 4, TIM_OCMODE_FORCED_INACTIVE = 4U << 4;
struct TIM_TypeDef
{
    uint32_t CR1{}, CR2{}, SMCR{}, DIER{}, CCER{}, CCMR1{}, CCMR2{}, PSC{}, ARR{}, CNT{}, CCR1{}, CCR2{},
      CCR3{}, CCR4{}, SR{};
    struct Event
    {
        TIM_TypeDef* timer;
        void operator=(uint32_t value)
        {
            if (value & TIM_EGR_UG) {
                timer->CNT = 0;
                timer->SR |= TIM_SR_UIF;
            }
        }
    } EGR{ this };
};
inline TIM_TypeDef timer;
inline auto* TIM2 = &timer;
struct DMA_Stream_TypeDef
{
    struct Control
    {
        uint32_t value{};
        operator uint32_t() const { return value; }
        void operator=(uint32_t next);
        void operator|=(uint32_t bits) { *this = value | bits; }
        void operator&=(uint32_t bits) { *this = value & bits; }
        void operator^=(uint32_t bits) { *this = value ^ bits; }
    } CR;
    uint32_t FCR{}, NDTR{};
    std::uintptr_t PAR{}, M0AR{}, M1AR{};
};
inline std::array<DMA_Stream_TypeDef, 4> dma_streams;
inline auto* DMA1_Stream0 = &dma_streams[0];
inline auto* DMA1_Stream1 = &dma_streams[1];
inline auto* DMA1_Stream2 = &dma_streams[2];
inline auto* DMA1_Stream3 = &dma_streams[3];
struct DMA_TypeDef
{
    uint32_t LISR{};
    struct Clear
    {
        DMA_TypeDef* dma;
        void operator=(uint32_t flags) { dma->LISR &= ~flags; }
    } LIFCR{ this };
};
inline DMA_TypeDef dma;
inline auto* DMA1 = &dma;
inline void DMA_Stream_TypeDef::Control::operator=(uint32_t next)
{
    // RM0433 15.3.15/16: software interruption also raises TCIF. It does
    // not imply that NDTR reached zero or that a double buffer completed.
    if ((value & 1U) && !(next & 1U)) {
        constexpr std::array shifts{ 0U, 6U, 16U, 22U };
        for (unsigned i = 0; i < dma_streams.size(); ++i) {
            if (this == &dma_streams[i].CR) {
                dma.LISR |= 0x20U << shifts[i];
            }
        }
    }
    value = next;
}
struct DMAMUX_Channel_TypeDef
{
    uint32_t CCR{};
};
inline std::array<DMAMUX_Channel_TypeDef, 4> mux;
#define DMAMUX1_Channel0_BASE reinterpret_cast<std::uintptr_t>(::mux.data())
constexpr uint32_t DMA_REQUEST_TIM2_CH1 = 18, DMA_REQUEST_TIM2_CH2 = 19, DMA_REQUEST_TIM2_CH3 = 20,
                   DMA_REQUEST_TIM2_CH4 = 21;
constexpr uint32_t DMA_SxCR_EN = 1, DMA_SxCR_DMEIE = 2, DMA_SxCR_TEIE = 4, DMA_SxCR_HTIE = 8,
                   DMA_SxCR_TCIE = 16;
constexpr uint32_t DMA_MEMORY_TO_PERIPH = 1U << 6, DMA_SxCR_CIRC = 1U << 8, DMA_SxCR_MINC = 1U << 10;
constexpr uint32_t DMA_PDATAALIGN_WORD = 2U << 11, DMA_MDATAALIGN_WORD = 2U << 13,
                   DMA_PRIORITY_VERY_HIGH = 3U << 16;
constexpr uint32_t DMA_SxCR_DBM = 1U << 18, DMA_SxCR_CT = 1U << 19, DMA_SxFCR_FEIE = 1U << 7;
enum IRQn_Type
{
    DMA1_Stream0_IRQn,
    DMA1_Stream1_IRQn,
    DMA1_Stream2_IRQn,
    DMA1_Stream3_IRQn
};
inline std::array<bool, 4> irq_enabled{};
inline uint32_t primask{};
inline uint32_t __get_PRIMASK() { return primask; }
inline void __disable_irq() { primask = 1; }
inline void __set_PRIMASK(uint32_t value) { primask = value; }
inline void __DMB() {}
inline void __DSB() {}
inline void HAL_NVIC_EnableIRQ(IRQn_Type irq) { irq_enabled[irq] = true; }
inline void HAL_NVIC_DisableIRQ(IRQn_Type irq) { irq_enabled[irq] = false; }
inline void HAL_NVIC_ClearPendingIRQ(IRQn_Type) {}
inline void HAL_NVIC_SetPriority(IRQn_Type, uint32_t, uint32_t) {}
inline bool tim2_clock_enabled{}, gpio_a_clock_enabled{}, gpio_b_clock_enabled{}, gpio_e_clock_enabled{};
inline void __HAL_RCC_TIM2_CLK_ENABLE() { tim2_clock_enabled = true; }
inline bool __HAL_RCC_TIM2_IS_CLK_ENABLED() { return tim2_clock_enabled; }
inline void __HAL_RCC_GPIOA_CLK_ENABLE() { gpio_a_clock_enabled = true; }
inline void __HAL_RCC_GPIOB_CLK_ENABLE() { gpio_b_clock_enabled = true; }
inline void __HAL_RCC_GPIOE_CLK_ENABLE() { gpio_e_clock_enabled = true; }
inline void __HAL_RCC_DMA1_CLK_ENABLE() {}
inline void __HAL_RCC_D2SRAM1_CLK_ENABLE() {}
struct RCC_ClkInitTypeDef
{
    uint32_t APB1CLKDivider{};
};
struct RCC_TypeDef
{
    uint32_t CFGR{};
};
inline RCC_TypeDef rcc;
inline auto* RCC = &rcc;
constexpr uint32_t RCC_CFGR_TIMPRE = 1, RCC_HCLK_DIV1 = 1, RCC_HCLK_DIV2 = 2, RCC_HCLK_DIV4 = 4;
inline void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef* clocks, uint32_t*)
{
    clocks->APB1CLKDivider = RCC_HCLK_DIV2;
}
inline uint32_t HAL_RCC_GetPCLK1Freq() { return 120'000'000; }
inline uint32_t HAL_RCC_GetHCLKFreq() { return 240'000'000; }
struct GPIO_TypeDef
{
    uint32_t level{}, IDR{};
    uint32_t MODER{}, OTYPER{}, OSPEEDR{}, PUPDR{};
    struct SetReset
    {
        GPIO_TypeDef* port;
        void operator=(uint32_t value)
        {
            port->level = (port->level & ~(value >> 16U)) | (value & 0xFFFFU);
            port->IDR = port->level;
        }
    } BSRR{ this };
    std::array<uint32_t, 16> mode{}, alternate{};
};
inline GPIO_TypeDef gpio_a, gpio_b, gpio_e;
inline auto* GPIOA = &gpio_a;
inline auto* GPIOB = &gpio_b;
inline auto* GPIOE = &gpio_e;
inline auto* M1_STEP_GPIO_Port = GPIOA;
inline auto* M2_STEP_GPIO_Port = GPIOB;
inline auto* M3_STEP_GPIO_Port = GPIOB;
inline auto* STEPPERS_EN_N_GPIO_Port = GPIOE;
constexpr uint32_t M1_STEP_Pin = 1, M2_STEP_Pin = 1U << 10, M3_STEP_Pin = 1U << 11,
                   STEPPERS_EN_N_Pin = 1U << 15;
constexpr uint32_t GPIO_PIN_0 = 1, GPIO_PIN_10 = 1U << 10, GPIO_PIN_11 = 1U << 11, GPIO_PIN_RESET = 0;
constexpr uint32_t GPIO_MODE_AF_PP = 2, GPIO_MODE_OUTPUT_PP = 1, GPIO_PULLDOWN = 2, GPIO_SPEED_FREQ_LOW = 0,
                   GPIO_AF1_TIM2 = 1;
struct GPIO_InitTypeDef
{
    uint32_t Pin{}, Mode{}, Pull{}, Speed{}, Alternate{};
};
inline void HAL_GPIO_WritePin(GPIO_TypeDef* gpio, uint32_t pins, uint32_t level)
{
    gpio->level = (gpio->level & ~pins) | (level ? pins : 0);
    gpio->IDR = (gpio->IDR & ~pins) | (level ? pins : 0);
}
inline void HAL_GPIO_Init(GPIO_TypeDef* gpio, const GPIO_InitTypeDef* config)
{
    for (unsigned pin = 0; pin < 16; ++pin)
        if (config->Pin & (1U << pin)) {
            gpio->mode[pin] = config->Mode;
            gpio->alternate[pin] = config->Alternate;
        }
}
