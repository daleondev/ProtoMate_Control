#include "PwmOutput.hpp"

#include "hal/drivers/common.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <utility>

namespace hal
{
    namespace
    {
        auto timer_handle(std::uint8_t timer) -> TIM_HandleTypeDef&
        {
            if (timer == 1U) {
                return htim1;
            }
            if (timer == 4U) {
                return htim4;
            }
            return htim8;
        }
        auto timer_input_hz(std::uint8_t timer) noexcept -> std::uint32_t
        {
            RCC_ClkInitTypeDef clocks{};
            std::uint32_t latency{};
            HAL_RCC_GetClockConfig(&clocks, &latency);
            const bool apb1{ timer == 4U };
            const auto divider{ apb1 ? clocks.APB1CLKDivider : clocks.APB2CLKDivider };
            auto hz{ apb1 ? HAL_RCC_GetPCLK1Freq() : HAL_RCC_GetPCLK2Freq() };
            if ((RCC->CFGR & RCC_CFGR_TIMPRE) != 0U) {
                if (divider == RCC_HCLK_DIV1 || divider == RCC_HCLK_DIV2 || divider == RCC_HCLK_DIV4) {
                    return HAL_RCC_GetHCLKFreq();
                }
                return hz * 4U;
            }
            return divider == RCC_HCLK_DIV1 ? hz : hz * 2U;
        }
    }
    PwmOutput::PwmOutput(pwm::Configuration configuration,
                         std::uint8_t alternate,
                         detail::TimerLease lease,
                         std::shared_ptr<IDigitalOutput> pin)
      : m_lease{ std::move(lease) }
      , m_pin{ std::move(pin) }
      , m_handle{ timer_handle(configuration.timer) }
      , m_port{ configuration.timer == 1U   ? GPIOE
                : configuration.timer == 4U ? GPIOD
                                            : GPIOC }
      , m_pinMask{ std::uint32_t{ 1U } << configuration.pin.number }
      , m_channel{ configuration.channel == 1U ? TIM_CHANNEL_1 : TIM_CHANNEL_3 }
      , m_alternate{ alternate }
      , m_inputHz{ timer_input_hz(configuration.timer) }
      , m_timer{ configuration.timer }
      , m_updateIrq{ m_timer == 1U   ? TIM1_UP_IRQn
                     : m_timer == 4U ? TIM4_IRQn
                                     : TIM8_UP_TIM13_IRQn }
      , m_compareIrq{ m_timer == 1U   ? TIM1_CC_IRQn
                      : m_timer == 4U ? TIM4_IRQn
                                      : TIM8_CC_IRQn }
      , m_compareFlag{ m_channel == TIM_CHANNEL_1 ? TIM_FLAG_CC1 : TIM_FLAG_CC3 }
    {
        const stm32::InterruptGuard guard;
        // CubeMX initializes the PWM peripheral. The driver owns its run state.
        static_cast<void>(stopImpl(false));
        s_instances[m_timer] = this;
    }
    PwmOutput::~PwmOutput()
    {
        const stm32::InterruptGuard guard;
        static_cast<void>(stopImpl(false));
        s_instances[m_timer] = nullptr;
    }

    auto PwmOutput::selectPinMode(bool alternate) noexcept -> void
    {
        // Keep the output latch low before connecting it to the pad.
        HAL_GPIO_WritePin(m_port, m_pinMask, GPIO_PIN_RESET);
        GPIO_InitTypeDef config{};
        config.Pin = m_pinMask;
        config.Mode = alternate ? GPIO_MODE_AF_PP : GPIO_MODE_OUTPUT_PP;
        config.Pull = GPIO_NOPULL;
        config.Speed = GPIO_SPEED_FREQ_LOW;
        config.Alternate = m_alternate;
        HAL_GPIO_Init(m_port, &config);
    }
    auto PwmOutput::configure(Timing requested) noexcept -> util::Result<>
    {
        const auto result{ detail::pwmTiming(requested, m_inputHz) };
        const stm32::InterruptGuard guard;
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        if (!result) {
            return std::unexpected(result.error());
        }
        m_timing = *result;
        return {};
    }
    auto PwmOutput::timing() const noexcept -> Timing
    {
        const stm32::InterruptGuard guard;
        return m_timing.actual;
    }
    auto PwmOutput::start() noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        if (m_running) {
            if (m_counted) {
                return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
            }
            return {};
        }
        if (m_timing.period_ticks == 0U) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        if (const auto result{ prepare(false) }; !result) {
            return result;
        }
        const auto result{ make_result(HAL_TIM_PWM_Start(&m_handle, m_channel)) };
        if (!result) {
            return result;
        }
        // The timer is now running at 0% duty while GPIO still drives low.
        // Connect AF first, then preload the pulse width. The first rising edge
        // occurs on a subsequent update, with a complete high phase.
        selectPinMode(true);
        __HAL_TIM_SET_COMPARE(&m_handle, m_channel, m_timing.high_ticks);
        m_train.start(0U);
        m_counted = false;
        m_running = true;
        return {};
    }
    auto PwmOutput::prepare(bool counted) noexcept -> util::Result<>
    {
        disableInterrupts();
        CLEAR_BIT(m_handle.Instance->CR1, TIM_CR1_CEN);
        MODIFY_REG(m_handle.Instance->CR1,
                   TIM_CR1_OPM | TIM_CR1_DIR | TIM_CR1_CMS | TIM_CR1_URS | TIM_CR1_UDIS,
                   counted ? TIM_CR1_OPM : 0U);
        m_handle.Instance->SMCR = 0U;
        if (m_timer != 4U) {
            m_handle.Instance->RCR = 0U;
        }
        __HAL_TIM_SET_PRESCALER(&m_handle, m_timing.prescaler);
        __HAL_TIM_SET_AUTORELOAD(&m_handle, m_timing.period_ticks - 1U);
        TIM_OC_InitTypeDef config{};
        // PWM2: low until CCR, then high until wrap. OPM stops at that wrap,
        // with CNT=0 and output LOW. PWM1 + OPM would leave the output HIGH.
        config.OCMode = counted ? TIM_OCMODE_PWM2 : TIM_OCMODE_PWM1;
        config.Pulse = counted ? m_timing.period_ticks - m_timing.high_ticks : 0U;
        config.OCPolarity = TIM_OCPOLARITY_HIGH;
        config.OCNPolarity = TIM_OCNPOLARITY_HIGH;
        config.OCFastMode = TIM_OCFAST_DISABLE;
        config.OCIdleState = TIM_OCIDLESTATE_RESET;
        config.OCNIdleState = TIM_OCNIDLESTATE_RESET;
        if (const auto result{ make_result(HAL_TIM_PWM_ConfigChannel(&m_handle, &config, m_channel)) };
            !result) {
            return result;
        }
        __HAL_TIM_ENABLE_OCxPRELOAD(&m_handle, m_channel);
        m_handle.Instance->SR = 0U;
        m_handle.Instance->EGR = TIM_EGR_UG; // Latch configuration and reset phase.
        // UG crosses into the timer clock domain. Wait for its acknowledgement
        // before clearing flags, otherwise a late UIF could look like a pulse end.
        while ((m_handle.Instance->SR & TIM_FLAG_UPDATE) == 0U) {
        }
        m_handle.Instance->SR = 0U; // Software UG must never count as a pulse.
        return {};
    }
    auto PwmOutput::startPulses(std::optional<PulseCount> count) noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        if (m_timing.period_ticks == 0U) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        if (count == 0U || m_timing.high_ticks == 0U || m_timing.high_ticks == m_timing.period_ticks) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        if (const auto result{ prepare(true) }; !result) {
            return result;
        }
        // Enable the inactive channel while CNT is still stopped at zero, so
        // selecting AF never connects the pad to a disabled/floating channel.
        TIM_CCxChannelCmd(m_handle.Instance, m_channel, TIM_CCx_ENABLE);
        if (m_timer != 4U) {
            __HAL_TIM_MOE_ENABLE(&m_handle);
        }
        selectPinMode(true);
        if (const auto result{ make_result(HAL_TIM_PWM_Start(&m_handle, m_channel)) }; !result) {
            selectPinMode(false);
            static_cast<void>(HAL_TIM_PWM_Stop(&m_handle, m_channel));
            return result;
        }
        m_train.start(count);
        m_counted = true;
        m_running = true;
        // With a callback, service the rising compare as well as the falling
        // update. Without one, the update ISR accounts for the latched compare.
        m_handle.Instance->DIER = TIM_IT_UPDATE | (m_callback ? m_compareFlag : 0U);
        HAL_NVIC_SetPriority(m_updateIrq, 5U, 0U);
        HAL_NVIC_EnableIRQ(m_updateIrq);
        if (m_callback) {
            HAL_NVIC_SetPriority(m_compareIrq, 5U, 0U);
            HAL_NVIC_EnableIRQ(m_compareIrq);
        }
        return {};
    }
    auto PwmOutput::disableInterrupts() noexcept -> void
    {
        m_handle.Instance->DIER = 0U;
        HAL_NVIC_DisableIRQ(m_updateIrq);
        HAL_NVIC_DisableIRQ(m_compareIrq);
        HAL_NVIC_ClearPendingIRQ(m_updateIrq);
        HAL_NVIC_ClearPendingIRQ(m_compareIrq);
    }
    auto PwmOutput::stopImpl(bool notify) noexcept -> util::Result<>
    {
        disableInterrupts();
        // Freeze before sampling flags: a concurrent rising edge is included.
        CLEAR_BIT(m_handle.Instance->CR1, TIM_CR1_CEN);
        selectPinMode(false); // Explicit push-pull low, even with timer outputs disabled.
        const auto result{ make_result(HAL_TIM_PWM_Stop(&m_handle, m_channel)) };
        const bool rose{ m_running && m_counted && (m_handle.Instance->SR & m_compareFlag) != 0U &&
                         m_train.rise() };
        m_handle.Instance->SR = 0U;
        m_running = false; // CEN cleared and pad forced low even if HAL reports an error.
        m_train.stop();
        if (notify && rose && m_callback) {
            m_callback(m_train.count());
        }
        return result;
    }
    auto PwmOutput::stop() noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        return stopImpl(true);
    }
    auto PwmOutput::pulseCount() const noexcept -> PulseCount
    {
        const stm32::InterruptGuard guard;
        return m_train.count(m_running && m_counted && (m_handle.Instance->SR & m_compareFlag) != 0U);
    }
    auto PwmOutput::setPulseCallback(PulseCallback callback) noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        m_callback = std::move(callback);
        return {};
    }
    auto PwmOutput::isRunning() const noexcept -> bool
    {
        const stm32::InterruptGuard guard;
        return m_running;
    }
    auto PwmOutput::handleInterrupt() noexcept -> void
    {
        if (!m_running || !m_counted) {
            return;
        }
        const auto pending{ m_handle.Instance->SR & (TIM_FLAG_UPDATE | m_compareFlag) };
        if ((pending & m_compareFlag) != 0U) {
            __HAL_TIM_CLEAR_FLAG(&m_handle, m_compareFlag);
            if (m_train.rise() && m_callback) {
                m_callback(m_train.count());
            }
        }
        if ((pending & TIM_FLAG_UPDATE) != 0U) {
            __HAL_TIM_CLEAR_FLAG(&m_handle, TIM_FLAG_UPDATE);
            // Hardware is already stopped low. No elapsed-time inference, and
            // no deadline by which this ISR must prevent an extra pulse.
            if (m_train.finishPulse()) {
                SET_BIT(m_handle.Instance->CR1, TIM_CR1_CEN);
            }
            else {
                static_cast<void>(stopImpl(false));
            }
        }
    }
    auto PwmOutput::dispatchInterrupt(std::uint8_t timer) noexcept -> void
    {
        const stm32::InterruptGuard guard;
        if (timer < s_instances.size() && s_instances[timer] != nullptr) {
            s_instances[timer]->handleInterrupt();
        }
    }
}

extern "C" void TIM1_UP_IRQHandler() { hal::PwmOutput::dispatchInterrupt(1U); }
extern "C" void TIM1_CC_IRQHandler() { hal::PwmOutput::dispatchInterrupt(1U); }
extern "C" void TIM4_IRQHandler() { hal::PwmOutput::dispatchInterrupt(4U); }
// TIM13 is unused; this driver owns the shared update vector while TIM8 is leased.
extern "C" void TIM8_UP_TIM13_IRQHandler() { hal::PwmOutput::dispatchInterrupt(8U); }
extern "C" void TIM8_CC_IRQHandler() { hal::PwmOutput::dispatchInterrupt(8U); }
