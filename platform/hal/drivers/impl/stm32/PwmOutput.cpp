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
      , m_pinMask{ 1UL << configuration.pin.number }
      , m_channel{ configuration.channel == 1U ? TIM_CHANNEL_1 : TIM_CHANNEL_3 }
      , m_alternate{ alternate }
      , m_inputHz{ timer_input_hz(configuration.timer) }
    {
        // CubeMX initializes the PWM peripheral. The driver owns its run state.
        static_cast<void>(stop());
    }
    PwmOutput::~PwmOutput() { static_cast<void>(stop()); }

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
            return {};
        }
        if (m_timing.period_ticks == 0U) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        __HAL_TIM_DISABLE(&m_handle);
        __HAL_TIM_SET_PRESCALER(&m_handle, m_timing.prescaler);
        __HAL_TIM_SET_AUTORELOAD(&m_handle, m_timing.period_ticks - 1U);
        __HAL_TIM_ENABLE_OCxPRELOAD(&m_handle, m_channel);
        __HAL_TIM_SET_COMPARE(&m_handle, m_channel, 0U);
        m_handle.Instance->EGR = TIM_EGR_UG; // Latch PSC/ARR/zero CCR; restart phase.
        __HAL_TIM_CLEAR_FLAG(&m_handle, TIM_FLAG_UPDATE);
        const auto result{ make_result(HAL_TIM_PWM_Start(&m_handle, m_channel)) };
        if (!result) {
            return result;
        }
        // The timer is now running at 0% duty while GPIO still drives low.
        // Connect AF first, then preload the pulse width. The first rising edge
        // occurs on a subsequent update, with a complete high phase.
        selectPinMode(true);
        __HAL_TIM_SET_COMPARE(&m_handle, m_channel, m_timing.high_ticks);
        m_running = true;
        return {};
    }
    auto PwmOutput::stop() noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        selectPinMode(false); // Explicit push-pull low, even with timer outputs disabled.
        const auto result{ make_result(HAL_TIM_PWM_Stop(&m_handle, m_channel)) };
        if (result) {
            m_running = false;
        }
        return result;
    }
    auto PwmOutput::isRunning() const noexcept -> bool
    {
        const stm32::InterruptGuard guard;
        return m_running;
    }
}
