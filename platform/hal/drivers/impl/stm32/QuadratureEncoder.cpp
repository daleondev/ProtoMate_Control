#include "QuadratureEncoder.hpp"

#include "hal/stm32/HalResult.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <utility>

namespace
{
    inline constexpr auto FIRST_SERVICE_COMPARE{ 0x5555U };
    inline constexpr auto SECOND_SERVICE_COMPARE{ 0xAAAAU };
    inline constexpr auto ENCODER_IRQ_PRIORITY{ 5U };
}

namespace hal
{
    namespace
    {
        constexpr std::uint32_t SERVICE_FLAGS{ TIM_FLAG_UPDATE | TIM_FLAG_CC3 | TIM_FLAG_CC4 };
        constexpr std::uint32_t SERVICE_INTERRUPTS{ TIM_IT_UPDATE | TIM_IT_CC3 | TIM_IT_CC4 };
        auto now() noexcept -> std::chrono::nanoseconds
        {
            return std::chrono::steady_clock::now().time_since_epoch();
        }
    }
    QuadratureEncoder::QuadratureEncoder(util::TimerLease lease,
                                         std::shared_ptr<IDigitalInput> a,
                                         std::shared_ptr<IDigitalInput> b)
      : m_lease{ std::move(lease) }
      , m_a{ std::move(a) }
      , m_b{ std::move(b) }
    {
        const stm32::InterruptGuard guard;
        HAL_NVIC_DisableIRQ(TIM3_IRQn);
        htim3.Instance->DIER = 0U;
        static_cast<void>(HAL_TIM_Encoder_Stop(&htim3, TIM_CHANNEL_ALL));
        __HAL_TIM_SET_COUNTER(&htim3, 0U);
        // CH1/CH2 remain CubeMX's x4 encoder inputs. CH3/CH4 are internal
        // frozen output-compare markers, with their external outputs disabled.
        // Together with wrap/update these divide the ring into gaps <=21846.
        // Service latency must be <10922 counts, so signed deltas stay <32768.
        htim3.Instance->CCMR2 = 0U;
        CLEAR_BIT(htim3.Instance->CCER, TIM_CCER_CC3E | TIM_CCER_CC4E);
        htim3.Instance->CCR3 = FIRST_SERVICE_COMPARE;
        htim3.Instance->CCR4 = SECOND_SERVICE_COMPARE;
        __HAL_TIM_CLEAR_FLAG(&htim3, SERVICE_FLAGS);
        HAL_NVIC_ClearPendingIRQ(TIM3_IRQn);
        s_instance = this;
    }
    QuadratureEncoder::~QuadratureEncoder()
    {
        const stm32::InterruptGuard guard;
        m_sampler.setCallback({});
        static_cast<void>(stop()); // NOLINT(bugprone-unused-return-value) -- Cleanup enforces stop; its final
                                   // status is not consumed here.
        s_instance = nullptr;
    }
    auto QuadratureEncoder::sample() const noexcept -> void
    {
        m_counter.sample(static_cast<std::uint16_t>(__HAL_TIM_GET_COUNTER(&htim3)));
    }
    auto QuadratureEncoder::start() noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        const auto count{ m_counter.position() };
        if (!count) {
            return std::unexpected(count.error());
        }
        if (m_running) {
            return {};
        }
        __HAL_TIM_CLEAR_FLAG(&htim3, SERVICE_FLAGS);
        HAL_NVIC_ClearPendingIRQ(TIM3_IRQn);
        const auto result{ make_result(HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL)) };
        if (!result) {
            return result;
        }
        m_running = true;
        sample();
        m_sampler.publish({ .position = m_counter.position(), .timestamp = now(), .running = true });
        // No CC1/CC2 interrupts: quadrature edges are counted by hardware.
        __HAL_TIM_ENABLE_IT(&htim3, SERVICE_INTERRUPTS);
        HAL_NVIC_SetPriority(TIM3_IRQn, ENCODER_IRQ_PRIORITY, 0U);
        HAL_NVIC_EnableIRQ(TIM3_IRQn);
        return {};
    }
    auto QuadratureEncoder::stop() noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        __HAL_TIM_DISABLE_IT(&htim3, SERVICE_INTERRUPTS);
        HAL_NVIC_DisableIRQ(TIM3_IRQn);
        const auto result{ make_result(HAL_TIM_Encoder_Stop(&htim3, TIM_CHANNEL_ALL)) };
        sample();
        HAL_NVIC_ClearPendingIRQ(TIM3_IRQn);
        if (result) {
            m_running = false;
            m_sampler.publish({ .position = m_counter.position(), .timestamp = now(), .running = false });
        }
        return result;
    }
    auto QuadratureEncoder::isRunning() const noexcept -> bool
    {
        const stm32::InterruptGuard guard;
        return m_running;
    }
    auto QuadratureEncoder::position() const noexcept -> util::Result<Count>
    {
        const stm32::InterruptGuard guard;
        sample();
        return m_counter.position();
    }
    auto QuadratureEncoder::setPosition(Count count) noexcept -> util::Result<>
    {
        const stm32::InterruptGuard guard;
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        m_counter.reset({ .count = count, .raw = static_cast<std::uint16_t>(__HAL_TIM_GET_COUNTER(&htim3)) });
        return {};
    }
    auto QuadratureEncoder::setSampleCallback(SampleCallback callback) -> void
    {
        const stm32::InterruptGuard guard;
        m_sampler.setCallback(std::move(callback));
        sample();
        m_sampler.publish({ .position = m_counter.position(), .timestamp = now(), .running = m_running });
    }
    auto QuadratureEncoder::dispatchTimebase() noexcept -> void
    {
        const stm32::InterruptGuard guard;
        if (s_instance == nullptr || !s_instance->m_running)
            return;
        const auto timestamp{ now() };
        if (s_instance->m_sampler.due(timestamp)) {
            s_instance->sample();
            s_instance->m_sampler.publish(
              { .position = s_instance->m_counter.position(), .timestamp = timestamp, .running = true });
        }
    }
    auto QuadratureEncoder::dispatchInterrupt() noexcept -> void
    {
        const stm32::InterruptGuard guard;
        const std::uint32_t pending{ htim3.Instance->SR & htim3.Instance->DIER & SERVICE_FLAGS };
        if (pending == 0U) {
            return;
        }
        // Clear before sampling. A later match stays pending and is serviced
        // again; bouncing across a wrap cannot corrupt an overflow-direction guess.
        __HAL_TIM_CLEAR_FLAG(&htim3, pending);
        if (s_instance != nullptr && s_instance->m_running) {
            s_instance->sample();
        }
    }
}

extern "C" void TIM3_IRQHandler() { hal::QuadratureEncoder::dispatchInterrupt(); }
