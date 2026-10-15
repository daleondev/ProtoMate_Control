#include "RgbOutput.hpp"
#include "hal/stm32/HalResult.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include <algorithm>
#include <exception>
#include <utility>
namespace
{
    hal::RgbOutput* instance{};
}
extern "C" void LTDC_IRQHandler()
{
    if (instance) {
        instance->interrupt();
    }
}
namespace hal
{
    RgbOutput::RgbOutput(LTDC_HandleTypeDef& lcd,
                         TIM_HandleTypeDef& pwm,
                         std::shared_ptr<IDigitalOutput> backlight)
      : m_lcd(lcd)
      , m_pwm(pwm)
      , m_backlight(std::move(backlight))
    {
        static char name[]{ "LTDC completion" };
        if (tx_semaphore_create(&m_completion, name, 0) != TX_SUCCESS) {
            std::terminate();
        }
    }
    RgbOutput::~RgbOutput()
    {
        shutdown();
        static_cast<void>(tx_semaphore_delete(&m_completion));
    }
    void RgbOutput::shutdown() noexcept
    {
        stm32::InterruptGuard guard;
        __HAL_TIM_SET_COMPARE(&m_pwm, TIM_CHANNEL_1, 0);
        __HAL_LTDC_DISABLE(&m_lcd);
        m_lcd.Instance->IER = 0;
        HAL_NVIC_DisableIRQ(LTDC_IRQn);
        HAL_NVIC_ClearPendingIRQ(LTDC_IRQn);
        m_callback = {};
        m_state.pending.reset();
        m_initialized = false;
        if (instance == this) {
            instance = nullptr;
        }
        static_cast<void>(tx_semaphore_ceiling_put(&m_completion, 1));
    }
    auto RgbOutput::initialize() -> util::Result<>
    {
        if (m_initialized) {
            return {};
        }
        __HAL_TIM_SET_COMPARE(&m_pwm, TIM_CHANNEL_1, 0);
        __HAL_LTDC_DISABLE(&m_lcd);
        for (unsigned slot = 0; slot < 2; ++slot) {
            std::ranges::fill(framebuffer(slot), 0);
        }
        // RM0399: AXI initiator 6 is LTDC. Its FIFO has a hard pixel-clock
        // deadline; CPU/DMA rendering can wait for SDRAM arbitration.
        GPV->AXI_INI6_READ_QOS = 15;
        // Timing/VSYNC runs immediately; fetch pixels only after the first
        // completed framebuffer is committed at vertical blank.
        __HAL_LTDC_LAYER_DISABLE(&m_lcd, 0);
        __HAL_LTDC_LAYER_DISABLE(&m_lcd, 1);
        auto result =
          HAL_LTDC_SetAddress_NoReload(&m_lcd, reinterpret_cast<std::uint32_t>(framebuffer(0).data()), 0);
        if (result != HAL_OK) {
            return make_result(result);
        }
        __HAL_LTDC_RELOAD_CONFIG(&m_lcd);
        {
            stm32::InterruptGuard guard;
            m_state = {};
            instance = this;
            m_lcd.Instance->ICR = LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF | LTDC_ICR_CRRIF | LTDC_ICR_CLIF;
            // First line of vertical front porch; no active pixels are scanned.
            m_lcd.Instance->LIPCR = m_lcd.Init.AccumulatedActiveH + 1;
            m_lcd.Instance->IER = LTDC_IT_FU | LTDC_IT_TE | LTDC_IT_LI | LTDC_IT_RR;
            HAL_NVIC_SetPriority(LTDC_IRQn, 10, 0);
            HAL_NVIC_EnableIRQ(LTDC_IRQn);
            __HAL_LTDC_ENABLE(&m_lcd);
            m_initialized = true;
        }
        result = HAL_TIM_PWM_Start(&m_pwm, TIM_CHANNEL_1);
        if (result != HAL_OK) {
            shutdown();
        }
        return make_result(result);
    }
    auto RgbOutput::framebuffer(unsigned slot) -> std::span<std::uint16_t>
    {
        if (slot > 1) {
            return {};
        }
        return { reinterpret_cast<std::uint16_t*>(0xC0000000u + slot * 0x200000u), 1024 * 600 };
    }
    auto RgbOutput::state() const noexcept -> display::State
    {
        stm32::InterruptGuard guard;
        return m_state;
    }
    void RgbOutput::setCallback(display::Callback callback) noexcept
    {
        stm32::InterruptGuard guard;
        m_callback = callback;
    }
    void RgbOutput::interrupt() noexcept
    {
        const auto flags = m_lcd.Instance->ISR & m_lcd.Instance->IER;
        m_lcd.Instance->ICR = flags;
        if (flags & LTDC_ISR_FUIF) {
            ++m_state.underruns;
        }
        if (flags & LTDC_ISR_TERRIF) {
            ++m_state.errors;
        }
        if (flags & (LTDC_ISR_FUIF | LTDC_ISR_TERRIF)) {
            m_callback(display::Event::Error);
        }
        if ((flags & LTDC_ISR_RRIF) && m_state.pending) {
            m_state.active = *m_state.pending;
            m_state.pending.reset();
            ++m_state.presentations;
            static_cast<void>(tx_semaphore_ceiling_put(&m_completion, 1));
            m_callback(display::Event::Presented);
        }
        if (flags & LTDC_ISR_LIF) {
            ++m_state.vsyncs;
            m_callback(display::Event::VSync);
        }
    }
    auto RgbOutput::requestPresent(unsigned slot) -> util::Result<>
    {
        stm32::InterruptGuard guard;
        if (!m_initialized || slot > 1 || slot == m_state.active) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        if (m_state.pending) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        __DSB();
        auto result =
          HAL_LTDC_SetAddress_NoReload(&m_lcd, reinterpret_cast<std::uint32_t>(framebuffer(slot).data()), 0);
        if (result == HAL_OK) {
            __HAL_LTDC_LAYER_ENABLE(&m_lcd, 0);
            m_state.pending = slot;
            // Reload-complete interrupt is the only authority returning the old
            // buffer to the renderer. A timeout never grants ownership.
            result = HAL_LTDC_Reload(&m_lcd, LTDC_RELOAD_VERTICAL_BLANKING);
            if (result != HAL_OK) {
                m_state.pending.reset();
            }
        }
        return make_result(result);
    }
    auto RgbOutput::present(unsigned slot) -> util::Result<>
    {
        const auto result = requestPresent(slot);
        if (!result) {
            return result;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 100 };
        while (state().pending) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     deadline - std::chrono::steady_clock::now())
                                     .count();
            if ((remaining <= 0 ||
                 tx_semaphore_get(&m_completion, static_cast<ULONG>(remaining) + 1) != TX_SUCCESS) &&
                state().pending) {
                return std::unexpected(std::make_error_code(std::errc::timed_out));
            }
        }
        return state().active == slot ? util::Result<>{}
                                      : std::unexpected(std::make_error_code(std::errc::operation_canceled));
    }
    auto RgbOutput::fill(unsigned slot, std::uint16_t color) -> util::Result<>
    {
        const auto current = state();
        if (slot > 1 || (m_initialized && (slot == current.active || current.pending == slot))) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        std::ranges::fill(framebuffer(slot), color);
        return {};
    }
    auto RgbOutput::backlight(unsigned percent) -> util::Result<>
    {
        if (!m_initialized || percent > 100) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        __HAL_TIM_SET_COMPARE(&m_pwm, TIM_CHANNEL_1, (m_pwm.Instance->ARR + 1u) * percent / 100u);
        return {};
    }
}
