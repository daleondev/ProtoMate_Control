#include "StepHardware.hpp"

#include "hal/drivers/detail/StepGenerator.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <array>

namespace hal::stm32
{
    namespace
    {
        struct alignas(32) DmaMemory
        {
            detail::StepBuffers edges;
            std::uint32_t stop_counter;
        };
        // Explicitly initialized on reset: this linker section is NOLOAD.
        __attribute__((section(".StepDmaSection"), aligned(32))) DmaMemory memory;
        detail::StepGenerator* generator{};
        const std::array streams{ DMA1_Stream0, DMA1_Stream1, DMA1_Stream2, DMA1_Stream3 };
        constexpr std::array<unsigned, 4> shifts{ 0U, 6U, 16U, 22U };
        constexpr std::uint32_t error_flags{ 0x0DU }; // FEIF, DMEIF, TEIF
        constexpr std::uint32_t half_flag{ 0x10U }, complete_flag{ 0x20U }, all_flags{ 0x3DU };
        constexpr std::array requests{ DMA_REQUEST_TIM2_CH1,
                                       DMA_REQUEST_TIM2_CH3,
                                       DMA_REQUEST_TIM2_CH4,
                                       DMA_REQUEST_TIM2_CH2 };
        constexpr std::array interrupts{ DMA1_Stream0_IRQn,
                                         DMA1_Stream1_IRQn,
                                         DMA1_Stream2_IRQn,
                                         DMA1_Stream3_IRQn };
        constexpr std::array<std::uint32_t, 3> cc_flags{ TIM_SR_CC1IF, TIM_SR_CC3IF, TIM_SR_CC4IF };
        constexpr std::array<std::uint32_t, 3> cc_dma{ TIM_DIER_CC1DE, TIM_DIER_CC3DE, TIM_DIER_CC4DE };
        constexpr std::array<std::uint32_t, 3> cc_enable{ TIM_CCER_CC1E, TIM_CCER_CC3E, TIM_CCER_CC4E };

        auto compare(std::size_t axis) noexcept -> volatile std::uint32_t&
        {
            if (axis == 0U) {
                return TIM2->CCR1;
            }
            if (axis == 1U) {
                return TIM2->CCR3;
            }
            return TIM2->CCR4;
        }
        auto disable(std::size_t index) noexcept -> void
        {
            CLEAR_BIT(streams[index]->CR, DMA_SxCR_EN);
            // RM0433: EN clears only once the in-flight transfer has settled.
            // The timer is stopped (or the axis parked) before reaching here.
            while ((streams[index]->CR & DMA_SxCR_EN) != 0U) {
            }
            __DSB();
        }
        auto pinMode(bool alternate) noexcept -> void
        {
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10 | GPIO_PIN_11, GPIO_PIN_RESET);
            GPIO_InitTypeDef config{};
            config.Mode = alternate ? GPIO_MODE_AF_PP : GPIO_MODE_OUTPUT_PP;
            config.Pull = GPIO_PULLDOWN;
            config.Speed = GPIO_SPEED_FREQ_LOW;
            config.Alternate = GPIO_AF1_TIM2;
            config.Pin = GPIO_PIN_0;
            HAL_GPIO_Init(GPIOA, &config);
            config.Pin = GPIO_PIN_10 | GPIO_PIN_11;
            HAL_GPIO_Init(GPIOB, &config);
        }
        class Hardware final : public detail::StepHardware
        {
          public:
            ~Hardware() override
            {
                const InterruptGuard lock;
                shutdown();
            }
            auto shutdown() noexcept -> void override
            {
                static_cast<void>(stop());
                generator = nullptr;
                for (const auto irq : interrupts) {
                    HAL_NVIC_DisableIRQ(irq);
                    HAL_NVIC_ClearPendingIRQ(irq);
                }
            }
            auto buffers() noexcept -> detail::StepBuffers& override { return memory.edges; }
            auto reset() noexcept -> bool override
            {
                static_cast<void>(stop());
                __HAL_RCC_TIM2_CLK_ENABLE();
                __HAL_RCC_DMA1_CLK_ENABLE();
                __HAL_RCC_D2SRAM1_CLK_ENABLE();
                // The CubeMX timer kernel is 240 MHz. Reject stale clock setup.
                RCC_ClkInitTypeDef clocks{};
                std::uint32_t latency{};
                HAL_RCC_GetClockConfig(&clocks, &latency);
                auto hz{ HAL_RCC_GetPCLK1Freq() };
                if ((RCC->CFGR & RCC_CFGR_TIMPRE) != 0U) {
                    hz = (clocks.APB1CLKDivider == RCC_HCLK_DIV1 || clocks.APB1CLKDivider == RCC_HCLK_DIV2 ||
                          clocks.APB1CLKDivider == RCC_HCLK_DIV4)
                           ? HAL_RCC_GetHCLKFreq()
                           : hz * 4U;
                }
                else if (clocks.APB1CLKDivider != RCC_HCLK_DIV1) {
                    hz *= 2U;
                }
                if (hz != 240'000'000U) {
                    return false;
                }
                TIM2->CR1 = 0U;
                TIM2->CR2 = 0U;
                TIM2->SMCR = 0U;
                TIM2->DIER = 0U;
                TIM2->CCER = 0U;
                // Force OCREF low first. Toggle mode retains this initial state.
                TIM2->CCMR1 = TIM_OCMODE_FORCED_INACTIVE;
                TIM2->CCMR2 = TIM_OCMODE_FORCED_INACTIVE | (TIM_OCMODE_FORCED_INACTIVE << 8U);
                TIM2->PSC = 23U;
                TIM2->ARR = detail::step_arr;
                TIM2->CNT = 0U;
                TIM2->CCR1 = detail::step_park;
                TIM2->CCR2 = detail::step_park;
                TIM2->CCR3 = detail::step_park;
                TIM2->CCR4 = detail::step_park;
                TIM2->EGR = TIM_EGR_UG;
                while ((TIM2->SR & TIM_SR_UIF) == 0U) {
                }
                TIM2->SR = 0U;
                m_enabled = m_requests = 0U;
                memory.stop_counter = 0U; // DMA writes CR1: CEN=0, no restart.
                for (std::size_t i = 0; i < streams.size(); ++i) {
                    DMA1->LIFCR = all_flags << shifts[i];
                    auto* mux{ reinterpret_cast<DMAMUX_Channel_TypeDef*>(
                      DMAMUX1_Channel0_BASE + i * sizeof(DMAMUX_Channel_TypeDef)) };
                    mux->CCR = requests[i];
                    auto* stream{ streams[i] };
                    stream->CR = DMA_MEMORY_TO_PERIPH | DMA_PDATAALIGN_WORD | DMA_MDATAALIGN_WORD |
                                 DMA_PRIORITY_VERY_HIGH | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
                    stream->FCR = DMA_SxFCR_FEIE;
                    HAL_NVIC_ClearPendingIRQ(interrupts[i]);
                    HAL_NVIC_SetPriority(interrupts[i], 5U, 0U);
                    HAL_NVIC_EnableIRQ(interrupts[i]);
                }
                auto* guard{ streams[3] };
                guard->PAR = reinterpret_cast<std::uintptr_t>(&TIM2->CR1);
                guard->M0AR = reinterpret_cast<std::uintptr_t>(&memory.stop_counter);
                guard->NDTR = 1U;
                return true;
            }
            auto arm(std::size_t axis, std::uint32_t first, std::uint32_t entries) noexcept -> void override
            {
                auto* stream{ streams[axis] };
                stream->PAR = reinterpret_cast<std::uintptr_t>(&compare(axis));
                stream->M0AR = reinterpret_cast<std::uintptr_t>(memory.edges[axis][0].data());
                stream->M1AR = reinterpret_cast<std::uintptr_t>(memory.edges[axis][1].data());
                stream->NDTR = entries;
                SET_BIT(stream->CR, DMA_SxCR_DBM | DMA_SxCR_CIRC | DMA_SxCR_MINC | DMA_SxCR_HTIE);
                compare(axis) = first;
                m_enabled |= cc_enable[axis];
                m_requests |= cc_dma[axis];
                SET_BIT(stream->CR, DMA_SxCR_EN);
            }
            auto start(std::uint32_t deadline) noexcept -> void override
            {
                TIM2->CCR2 = deadline;
                TIM2->CCMR1 = TIM_OCMODE_TOGGLE; // CH2 internal timing, no output/preload.
                TIM2->CCMR2 = TIM_OCMODE_TOGGLE | (TIM_OCMODE_TOGGLE << 8U);
                TIM2->CCER = m_enabled;
                pinMode(true);
                SET_BIT(streams[3]->CR, DMA_SxCR_EN);
                TIM2->DIER = m_requests | TIM_DIER_CC2DE;
                __DSB();
                TIM2->CR1 = TIM_CR1_CEN;
            }
            auto guard(std::uint32_t tick) noexcept -> void override
            {
                TIM2->CCR2 = tick;
                __DSB();
            }
            auto publish() noexcept -> void override { __DMB(); }
            auto sample() noexcept -> detail::StepSample override
            {
                detail::StepSample result;
                for (std::size_t i = 0; i < 3U; ++i) {
                    auto& channel{ result.channels[i] };
                    auto* stream{ streams[i] };
                    std::uint32_t before{}, flags{}, after{}, control{};
                    // Retry if DMA progressed while reading CCR/CNT. NDTR and
                    // CT together disambiguate the double-buffer reload boundary.
                    do {
                        control = stream->CR;
                        before = stream->NDTR;
                        flags = DMA1->LISR;
                        channel.compare = compare(i);
                        channel.tick = TIM2->CNT;
                        channel.compare_pending = (TIM2->SR & cc_flags[i]) != 0U;
                        channel.high = i == 0U ? (GPIOA->IDR & GPIO_PIN_0) != 0U
                                               : (GPIOB->IDR & (i == 1U ? GPIO_PIN_10 : GPIO_PIN_11)) != 0U;
                        after = stream->NDTR;
                    } while (before != after || ((control ^ stream->CR) & DMA_SxCR_CT) != 0U ||
                             ((flags ^ DMA1->LISR) & (complete_flag << shifts[i])) != 0U);
                    channel.remaining = after;
                    channel.target = (control & DMA_SxCR_CT) != 0U ? 1U : 0U;
                    channel.transfer_complete = (flags & (complete_flag << shifts[i])) != 0U;
                }
                const auto flags{ DMA1->LISR };
                for (auto shift : shifts) {
                    result.error |= (flags & (error_flags << shift)) != 0U;
                }
                result.tick = TIM2->CNT;
                result.running = (TIM2->CR1 & TIM_CR1_CEN) != 0U;
                return result;
            }
            auto acknowledge(std::size_t axis) noexcept -> void override
            {
                DMA1->LIFCR = (complete_flag | half_flag) << shifts[axis];
            }
            auto finishAxis(std::size_t axis) noexcept -> void override
            {
                CLEAR_BIT(TIM2->DIER, cc_dma[axis]);
                disable(axis);
                // The final falling compare may still be awaiting DMA. Do not
                // leave that old timestamp able to toggle again after wrap.
                compare(axis) = detail::step_park;
                acknowledge(axis);
            }
            auto stop() noexcept -> detail::StepSample override
            {
                CLEAR_BIT(TIM2->CR1, TIM_CR1_CEN);
                __DSB();
                TIM2->DIER = 0U;
                for (std::size_t i = 0; i < streams.size(); ++i) {
                    disable(i);
                }
                const auto result{ sample() };
                TIM2->CCER = 0U;
                pinMode(false);
                return result;
            }

          private:
            std::uint32_t m_enabled{}, m_requests{};
        };
    }
    auto makeStepHardware() -> std::unique_ptr<detail::StepHardware> { return std::make_unique<Hardware>(); }
    auto registerStepGenerator(detail::StepGenerator* value) noexcept -> void { generator = value; }
    auto stepInterrupt() noexcept -> void
    {
        const InterruptGuard lock;
        // Clear HT flags only; the core consumes TC together with CT/NDTR.
        DMA1->LIFCR = (half_flag << shifts[0]) | (half_flag << shifts[1]) | (half_flag << shifts[2]);
        if (generator != nullptr) {
            generator->service();
        }
        // Finished/failed generators must not leave a level-sensitive IRQ pending.
        if ((TIM2->CR1 & TIM_CR1_CEN) == 0U) {
            DMA1->LIFCR = (all_flags << shifts[0]) | (all_flags << shifts[1]) | (all_flags << shifts[2]) |
                          (all_flags << shifts[3]);
        }
    }
}

extern "C" void DMA1_Stream0_IRQHandler() { hal::stm32::stepInterrupt(); }
extern "C" void DMA1_Stream1_IRQHandler() { hal::stm32::stepInterrupt(); }
extern "C" void DMA1_Stream2_IRQHandler() { hal::stm32::stepInterrupt(); }
extern "C" void DMA1_Stream3_IRQHandler() { hal::stm32::stepInterrupt(); }
