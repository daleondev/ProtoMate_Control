#include "RgbOutput.hpp"
#include "hal/stm32/HalResult.hpp"
#include <utility>
namespace { volatile std::uint32_t underrun_count{}; }
extern "C" void LTDC_IRQHandler()
{
    const auto flags = LTDC->ISR;
    if (flags & LTDC_ISR_FUIF) { underrun_count = underrun_count + 1; }
    LTDC->ICR = flags & (LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF | LTDC_ICR_CRRIF | LTDC_ICR_CLIF);
}
namespace hal {
    RgbOutput::RgbOutput(LTDC_HandleTypeDef& lcd, DMA2D_HandleTypeDef& dma, TIM_HandleTypeDef& pwm,
        std::shared_ptr<IDigitalOutput> backlight)
      : m_lcd(lcd), m_dma(dma), m_pwm(pwm), m_backlight(std::move(backlight)) {}
    RgbOutput::~RgbOutput()
    {
        __HAL_TIM_SET_COMPARE(&m_pwm, TIM_CHANNEL_1, 0);
        __HAL_LTDC_DISABLE(&m_lcd);
        HAL_NVIC_DisableIRQ(LTDC_IRQn);
    }
    auto RgbOutput::initialize() -> util::Result<>
    {
        if (m_initialized) { return {}; }
        __HAL_TIM_SET_COMPARE(&m_pwm, TIM_CHANNEL_1, 0);
        // SDRAM is already initialized and mapped non-cacheable by bootstrap.
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto result = fill(slot, 0);
            if (!result) { return result; }
        }
        __HAL_LTDC_LAYER_DISABLE(&m_lcd, 1);
        __HAL_LTDC_RELOAD_CONFIG(&m_lcd);
        LTDC->ICR = LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF | LTDC_ICR_CRRIF | LTDC_ICR_CLIF;
        HAL_NVIC_SetPriority(LTDC_IRQn, 10, 0);
        HAL_NVIC_EnableIRQ(LTDC_IRQn);
        __HAL_LTDC_ENABLE_IT(&m_lcd, LTDC_IT_FU | LTDC_IT_TE);
        __HAL_LTDC_ENABLE(&m_lcd);
        const auto result = make_result(HAL_TIM_PWM_Start(&m_pwm, TIM_CHANNEL_1));
        if (!result) { return result; }
        m_initialized = true;
        return {};
    }
    auto RgbOutput::framebuffer(unsigned slot) -> std::span<std::uint16_t>
    {
        if (slot > 1) { return {}; }
        return {reinterpret_cast<std::uint16_t*>(0xC0000000u + slot * 0x200000u), 1024 * 600};
    }
    auto RgbOutput::fill(unsigned slot, std::uint16_t color) -> util::Result<>
    {
        if (slot > 1 || (m_initialized && slot == m_active)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        // DMA2D R2M uses ARGB8888 input color even for an RGB565 output buffer.
        const auto red = ((color >> 11) & 31u) * 255u / 31u;
        const auto green = ((color >> 5) & 63u) * 255u / 63u;
        const auto blue = (color & 31u) * 255u / 31u;
        m_dma.Init.Mode = DMA2D_R2M;
        m_dma.Init.ColorMode = DMA2D_OUTPUT_RGB565;
        m_dma.Init.OutputOffset = 0;
        auto result = HAL_DMA2D_Init(&m_dma);
        if (result == HAL_OK) { result = HAL_DMA2D_Start(&m_dma, 0xFF000000u | red << 16 | green << 8 | blue,
            reinterpret_cast<std::uint32_t>(framebuffer(slot).data()), 1024, 600); }
        if (result == HAL_OK) { result = HAL_DMA2D_PollForTransfer(&m_dma, 100); }
        if (result != HAL_OK) { static_cast<void>(HAL_DMA2D_Abort(&m_dma)); }
        return make_result(result);
    }
    auto RgbOutput::present(unsigned slot) -> util::Result<>
    {
        if (!m_initialized || slot > 1) { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }
        __DSB();
        auto result = HAL_LTDC_SetAddress_NoReload(&m_lcd, reinterpret_cast<std::uint32_t>(framebuffer(slot).data()), 0);
        if (result == HAL_OK) { result = HAL_LTDC_Reload(&m_lcd, LTDC_RELOAD_VERTICAL_BLANKING); }
        if (result != HAL_OK) { return make_result(result); }
        const auto start = HAL_GetTick();
        while (LTDC->SRCR & LTDC_SRCR_VBR) {
            if (HAL_GetTick() - start >= 50) { return std::unexpected(std::make_error_code(std::errc::timed_out)); }
        }
        m_active = slot;
        return {};
    }
    auto RgbOutput::backlight(unsigned percent) -> util::Result<>
    {
        if (!m_initialized || percent > 100) { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }
        __HAL_TIM_SET_COMPARE(&m_pwm, TIM_CHANNEL_1, (m_pwm.Instance->ARR + 1u) * percent / 100u);
        return {};
    }
    auto RgbOutput::underruns() const noexcept -> std::uint32_t { return underrun_count; }
}
