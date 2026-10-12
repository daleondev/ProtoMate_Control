// CMSIS/HAL macros intentionally use C casts, fixed MMIO addresses and do-while wrappers.
// The line-specific exceptions below preserve the vendor register interface.
#include "StepHardware.hpp"

#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/stm32/HardwareSemaphore.hpp"
#include "hal/stm32/TimerClock.hpp"

#include <array>

namespace
{
    inline constexpr auto DMA_CACHE_LINE_BYTES{ 32 };
    inline constexpr auto CAPTURE_COMPARE_MODE_MASK{ 0xFFU };
    inline constexpr auto CAPTURE_COMPARE_FIELD_SHIFT{ 16U };
    inline constexpr auto STEP_TIMER_FREQUENCY_HZ{ 1'000'000'000U / hal::util::STEP_TICK_NS };
    inline constexpr auto CAPTURE_COMPARE_FIELD_BITS{ 8U };
    inline constexpr auto STEP_IRQ_PRIORITY{ 5U };
    inline constexpr auto GUARD_TIMER_FREQUENCY_HZ{ 1'000'000U };
    inline constexpr auto GUARD_TIMER_RELOAD{ 999U };
}

namespace hal::stm32
{
    namespace
    {
        struct alignas(DMA_CACHE_LINE_BYTES) DmaMemory
        {
            util::StepBuffers edges;
            std::uint32_t stop_counter;
        };
        // Explicitly initialized on reset: this linker section is NOLOAD.
        __attribute__((section(".StepDmaSection"), aligned(DMA_CACHE_LINE_BYTES))) DmaMemory
          memory; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) -- Fixed DMA/IRQ state,
                  // serialized by InterruptGuard.
        util::StepGenerator* generator{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) --
                                          // Fixed DMA/IRQ state, serialized by InterruptGuard.
        const std::array STREAMS{
            DMA1_Stream0, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            DMA1_Stream1, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            DMA1_Stream2, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            DMA1_Stream3 // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        constexpr std::array<unsigned, 4> SHIFTS{ 0U, 6U, 16U, 22U };
        constexpr std::uint32_t ERROR_FLAGS{ 0x0DU }; // FEIF, DMEIF, TEIF
        constexpr std::uint32_t HALF_FLAG{ 0x10U }, COMPLETE_FLAG{ 0x20U }, ALL_FLAGS{ 0x3DU };
        constexpr std::array REQUESTS{ DMA_REQUEST_TIM2_CH1,
                                       DMA_REQUEST_TIM2_CH3,
                                       DMA_REQUEST_TIM2_CH4,
                                       DMA_REQUEST_TIM2_CH2 };
        constexpr std::array INTERRUPTS{ DMA1_Stream0_IRQn,
                                         DMA1_Stream1_IRQn,
                                         DMA1_Stream2_IRQn,
                                         DMA1_Stream3_IRQn };
        constexpr std::array<std::uint32_t, 3> CC_FLAGS{ TIM_SR_CC1IF, TIM_SR_CC3IF, TIM_SR_CC4IF };
        constexpr std::array<std::uint32_t, 3> CC_DMA{ TIM_DIER_CC1DE, TIM_DIER_CC3DE, TIM_DIER_CC4DE };
        constexpr std::array<std::uint32_t, 3> CC_ENABLE{ TIM_CCER_CC1E, TIM_CCER_CC3E, TIM_CCER_CC4E };

        auto compare(std::size_t axis) noexcept -> volatile std::uint32_t&
        {
            if (axis == 0U) {
                return TIM2->CCR1; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            }
            if (axis == 1U) {
                return TIM2->CCR3; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            }
            return TIM2->CCR4; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        auto disable(std::size_t index) noexcept -> void
        {
            CLEAR_BIT(STREAMS[index]->CR, DMA_SxCR_EN);
            // RM0433: EN clears only once the in-flight transfer has settled.
            // The timer is stopped (or the axis parked) before reaching here.
            while ((STREAMS[index]->CR & DMA_SxCR_EN) != 0U) {
            }
            __DSB();
        }
        auto pin_mode(std::size_t axis, bool alternate) noexcept -> void
        {
            const HardwareSemaphore shared(5);
            if (!shared) { hal::panic("STEP GPIO configuration lock timeout"); }
            auto* port{
                axis == 0U ? GPIOA : GPIOB // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            }; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            constexpr std::array pins{ GPIO_PIN_0, GPIO_PIN_10, GPIO_PIN_11 };
            const auto pin{ pins[axis] };
            HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
            GPIO_InitTypeDef config{};
            config.Mode = alternate ? GPIO_MODE_AF_PP : GPIO_MODE_OUTPUT_PP;
            config.Pull = GPIO_PULLDOWN;
            config.Speed = GPIO_SPEED_FREQ_LOW;
            config.Alternate = GPIO_AF1_TIM2;
            config.Pin = pin;
            HAL_GPIO_Init(port, &config);
        }
        enum class OutputMode : std::uint8_t
        {
            Inactive = TIM_OCMODE_FORCED_INACTIVE,
            Toggle = TIM_OCMODE_TOGGLE
        };
        auto output_mode(std::size_t axis, OutputMode mode) noexcept -> void
        {
            auto& reg{ // NOLINT(clang-analyzer-core.FixedAddressDereference)
                axis == 0U ? TIM2->CCMR1 : TIM2->CCMR2 // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            }; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            const auto shift{ axis == 2U ? CAPTURE_COMPARE_FIELD_BITS : 0U };
            // Preserve the other channel sharing this CCMR register.
            reg = (reg & ~((CAPTURE_COMPARE_MODE_MASK | (1U << CAPTURE_COMPARE_FIELD_SHIFT)) << shift)) |
                  (std::to_underlying(mode) << shift);
        }

    }
    StepHardware::~StepHardware()
    {
        const InterruptGuard lock;
        shutdown();
    }
    auto StepHardware::shutdown() noexcept -> void
    {
        static_cast<void>(stop());
        generator = nullptr;
        for (const auto irq : INTERRUPTS) {
            HAL_NVIC_DisableIRQ(irq);
            HAL_NVIC_ClearPendingIRQ(irq);
        }
        HAL_NVIC_DisableIRQ(TIM7_IRQn);
        HAL_NVIC_ClearPendingIRQ(TIM7_IRQn);
    }
    auto StepHardware::buffers() noexcept -> util::StepBuffers& { return memory.edges; }
    auto StepHardware::reset() noexcept -> bool
    {
        static_cast<void>(stop());
        {
            const HardwareSemaphore shared(5);
            if (!shared) { return false; }
            __HAL_RCC_TIM2_CLK_ENABLE(); // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            __HAL_RCC_TIM7_CLK_ENABLE(); // NOLINT(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            __HAL_RCC_DMA1_CLK_ENABLE(); // NOLINT(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            __HAL_RCC_D2SRAM1_CLK_ENABLE(); // NOLINT(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        // Preserve the common 100 ns scheduling unit across clock configurations.
        const auto hz = apb1_timer_input_frequency_hz();
        if (hz < STEP_TIMER_FREQUENCY_HZ || hz % STEP_TIMER_FREQUENCY_HZ != 0U ||
            hz / STEP_TIMER_FREQUENCY_HZ > 65'536U ||
            hz % GUARD_TIMER_FREQUENCY_HZ != 0U || hz / GUARD_TIMER_FREQUENCY_HZ > 65'536U) {
            return false;
        }
        TIM2->CR1 = 0U;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CR2 = 0U;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->SMCR = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->DIER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CCER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        // Force OCREF low first. Toggle mode retains this initial state.
        TIM2->CCMR1 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM_OCMODE_FORCED_INACTIVE; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CCMR2 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM_OCMODE_FORCED_INACTIVE |
          (TIM_OCMODE_FORCED_INACTIVE
           << CAPTURE_COMPARE_FIELD_BITS); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->PSC = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          hz / STEP_TIMER_FREQUENCY_HZ - 1U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->ARR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          util::STEP_ARR; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CNT = 0U;   // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CCR1 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          util::STEP_PARK; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CCR2 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          util::STEP_PARK; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CCR3 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          util::STEP_PARK; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->CCR4 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          util::STEP_PARK;      // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->EGR = TIM_EGR_UG; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        while ((TIM2->SR & TIM_SR_UIF) == // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
               0U) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        TIM2->SR = 0U;            // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        memory.stop_counter = 0U;
        publish(); // DMA writes CR1: CEN=0, no restart.
        for (std::size_t i = 0; i < STREAMS.size(); ++i) {
            DMA1->LIFCR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              ALL_FLAGS
              << SHIFTS[i]; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            auto* mux{ reinterpret_cast<DMAMUX_Channel_TypeDef*>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
              DMAMUX1_Channel0_BASE + // NOLINT(performance-no-int-to-ptr) //
                                      // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- DMA address
                                      // register/cache-maintenance ABI.
              (i * sizeof(DMAMUX_Channel_TypeDef))) };
            mux->CCR = REQUESTS[i];
            auto* stream{ STREAMS[i] };
            stream->CR = DMA_MEMORY_TO_PERIPH | DMA_PDATAALIGN_WORD | DMA_MDATAALIGN_WORD |
                         DMA_PRIORITY_VERY_HIGH | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
            stream->FCR = DMA_SxFCR_FEIE;
            HAL_NVIC_ClearPendingIRQ(INTERRUPTS[i]);
            HAL_NVIC_SetPriority(INTERRUPTS[i], STEP_IRQ_PRIORITY, 0U);
            HAL_NVIC_EnableIRQ(INTERRUPTS[i]);
        }
        auto* guard{ STREAMS[3] };
        guard->PAR = reinterpret_cast<std::uintptr_t>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          &TIM2->CR1); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr) //
                       // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- DMA address
                       // register/cache-maintenance ABI.
        guard->M0AR = reinterpret_cast<std::uintptr_t>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          &memory.stop_counter); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- DMA address
                                 // register/cache-maintenance ABI.
        guard->NDTR = 1U;
        TIM7->CR1 = 0U;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM7->DIER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM7->PSC = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          hz / GUARD_TIMER_FREQUENCY_HZ - 1U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM7->ARR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          GUARD_TIMER_RELOAD;   // 1 ms completion monitor; no output pins. //
                                // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM7->EGR = TIM_EGR_UG; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        while ((TIM7->SR & TIM_SR_UIF) == // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
               0U) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        TIM7->SR = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        HAL_NVIC_SetPriority(TIM7_IRQn, STEP_IRQ_PRIORITY, 0U);
        HAL_NVIC_ClearPendingIRQ(TIM7_IRQn);
        HAL_NVIC_EnableIRQ(TIM7_IRQn);
        return true;
    }
    auto StepHardware::arm(std::size_t axis, std::uint32_t first, std::uint32_t entries) noexcept -> bool
    {
        auto* stream{ STREAMS[axis] };
        CLEAR_BIT( // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM2->DIER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          CC_DMA
            [axis]); // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        CLEAR_BIT( // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM2->CCER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          CC_ENABLE[axis]); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        disable(axis);
        compare(axis) = util::STEP_PARK;
        output_mode(axis, OutputMode::Inactive);
        pin_mode(axis, true);
        DMA1->LIFCR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          ALL_FLAGS
          << SHIFTS[axis]; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->SR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          ~CC_FLAGS[axis]; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        CLEAR_BIT(stream->CR, DMA_SxCR_CT);
        stream->PAR = reinterpret_cast<std::uintptr_t>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          &compare(axis)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                           // -- DMA address register/cache-maintenance ABI.
        stream->M0AR = reinterpret_cast<std::uintptr_t>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          memory.edges[axis][0].data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- DMA address
                                         // register/cache-maintenance ABI.
        stream->M1AR = reinterpret_cast<std::uintptr_t>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          memory.edges[axis][1].data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- DMA address
                                         // register/cache-maintenance ABI.
        stream->NDTR = entries;
        SET_BIT(stream->CR, DMA_SxCR_DBM | DMA_SxCR_CIRC | DMA_SxCR_MINC | DMA_SxCR_HTIE);
        const auto lead{ util::step_distance(
          TIM2->CNT, first) }; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        if ((TIM2->CR1 & TIM_CR1_CEN) == 0U || lead < util::STEP_MIN_PHASE || // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            lead >
              util::
                STEP_HORIZON) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            pin_mode(axis, false);
            return false;
        }
        SET_BIT(stream->CR, DMA_SxCR_EN);
        output_mode(axis, OutputMode::Toggle);
        SET_BIT(TIM2->CCER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                CC_ENABLE[axis]); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        SET_BIT(TIM2->DIER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                CC_DMA[axis]); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        __DSB();
        compare(axis) = first; // Last: expose a future edge on the running counter.
        return true;
    }
    auto StepHardware::start(std::uint32_t deadline) noexcept -> void
    {
        SET_BIT(STREAMS[3]->CR, DMA_SxCR_EN);
        guard(deadline);
        __DSB();
        TIM2->CR1 = // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM_CR1_CEN; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    }
    auto StepHardware::guard(std::uint32_t tick) noexcept -> void
    {
        if (tick == util::STEP_PARK) {
            // CCR > ARR can still generate a compare request at wrap!
            CLEAR_BIT( // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              TIM2->DIER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              TIM_DIER_CC2DE); // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM2->CCR2 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              util::STEP_PARK; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        else {
            TIM2->CCR2 = // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              tick; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            SET_BIT( // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              TIM2->DIER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              TIM_DIER_CC2DE); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        __DSB();
    }
    auto StepHardware::publish() noexcept -> void
    {
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
        // M4 has no data cache. Keep direct board access correct on M7 too.
        SCB_CleanDCache_by_Addr(&memory, static_cast<std::int32_t>(sizeof(memory)));
#endif
        __DMB();
    }
    auto StepHardware::completionWatch(bool enabled) noexcept -> void
    {
        if (!enabled) {
            TIM7->CR1 = // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              0U; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM7->DIER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        else if ((TIM7->CR1 & TIM_CR1_CEN) == // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                 0U) {      // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM7->CNT = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM7->SR = 0U;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM7->DIER = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              TIM_DIER_UIE; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM7->CR1 = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              TIM_CR1_CEN; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
    }
    auto StepHardware::sample() noexcept -> util::StepSample
    {
        util::StepSample result;
        for (std::size_t i = 0; i < 3U; ++i) {
            auto& channel{ result.channels[i] };
            auto* stream{ STREAMS[i] };
            std::uint32_t before{};
            std::uint32_t flags{};
            std::uint32_t after{};
            std::uint32_t control{};
            // Retry if DMA progressed while reading CCR/CNT. NDTR and
            // CT together disambiguate the double-buffer reload boundary.
            while (true) {
                control = stream->CR;
                before = stream->NDTR;
                flags =
                  DMA1 // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                    ->LISR; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                channel.compare = compare(i);
                channel.tick =
                  TIM2->CNT; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                channel.compare_pending =
                  (TIM2->SR & CC_FLAGS[i]) != // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                  0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                channel.high =
                  i == 0U ? (GPIOA->IDR & GPIO_PIN_0) != // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                              0U // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                          : (GPIOB->IDR & (i == 1U ? GPIO_PIN_10 : GPIO_PIN_11)) != // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                              0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                after = stream->NDTR;
                if (before == after && ((control ^ stream->CR) & DMA_SxCR_CT) == 0U &&
                    ((flags ^ DMA1->LISR) & (COMPLETE_FLAG << SHIFTS[i])) == // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                      0U) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                    break;
                }
            }
            channel.remaining = after;
            channel.target = (control & DMA_SxCR_CT) != 0U ? 1U : 0U;
            channel.transfer_complete = (flags & (COMPLETE_FLAG << SHIFTS[i])) != 0U;
        }
        const auto flags{
            DMA1->LISR // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        for (auto shift : SHIFTS) {
            result.error |= (flags & (ERROR_FLAGS << shift)) != 0U;
        }
        result.tick = TIM2->CNT; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        result.running = (TIM2->CR1 & TIM_CR1_CEN) != // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                         0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        return result;
    }
    auto StepHardware::acknowledge(std::size_t axis) noexcept -> void
    {
        DMA1->LIFCR = // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          (COMPLETE_FLAG | HALF_FLAG) << SHIFTS
            [axis]; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    }
    auto StepHardware::stopAxis(std::size_t axis) noexcept -> util::StepSample
    {
        // Keep the pad connected while disabling requests and settling
        // DMA. The remaining compare can occur once; sample its phase
        // after parking so that edge is included in the final count.
        CLEAR_BIT( // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM2->DIER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          CC_DMA
            [axis]); // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        const auto before{ sample().channels[axis] };
        disable(axis);
        const auto last_compare{ compare(axis) };
        compare(axis) = util::STEP_PARK;
        __DSB();
        auto result{ sample() };
        auto& after{ result.channels[axis] };
        after.compare = last_compare;
        after.transfer_complete = before.transfer_complete || after.target != before.target;
        CLEAR_BIT( // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM2->CCER, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          CC_ENABLE[axis]); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        pin_mode(axis, false);
        output_mode(axis, OutputMode::Inactive);
        DMA1->LIFCR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          ALL_FLAGS
          << SHIFTS[axis]; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        TIM2->SR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          ~CC_FLAGS[axis]; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        return result;
    }
    auto StepHardware::finishAxis(std::size_t axis) noexcept -> void { static_cast<void>(stopAxis(axis)); }
    auto StepHardware::stop() noexcept -> util::StepSample
    {
        completionWatch(false);
        CLEAR_BIT( // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM2->CR1, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          TIM_CR1_CEN); // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        __DSB();
        TIM2->DIER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        const auto before_disable{ sample() };
        for (std::size_t i = 0; i < STREAMS.size(); ++i) {
            disable(i);
        }
        auto result{ sample() };
        for (std::size_t i = 0; i < result.channels.size(); ++i) {
            // RM0433 15.3.15/16: clearing EN itself raises TCIF even
            // for a partial buffer. Preserve genuine completion flags
            // from before shutdown. With CEN stopped, at most one
            // pending edge can settle; a CT change captures a real
            // buffer completion during the disable handshake.
            auto& after{ result.channels[i] };
            const auto& before{ before_disable.channels[i] };
            after.transfer_complete = before.transfer_complete || after.target != before.target;
        }
        TIM2->CCER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        for (std::size_t i = 0; i < 3U; ++i) {
            pin_mode(i, false);
        }
        return result;
    }
    auto register_step_generator(util::StepGenerator* value) noexcept -> void { generator = value; }
    auto step_interrupt() noexcept -> void
    {
        const InterruptGuard lock;
        // Clear HT flags only; the core consumes TC together with CT/NDTR.
        DMA1->LIFCR = // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          (HALF_FLAG << SHIFTS[0]) | (HALF_FLAG << SHIFTS[1]) |
          (HALF_FLAG << SHIFTS
             [2]); // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        if (generator != nullptr) {
            generator->service();
        }
        // Finished/failed generators must not leave a level-sensitive IRQ pending.
        if ((TIM2->CR1 & TIM_CR1_CEN) == // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            0U) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            DMA1->LIFCR = // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              (ALL_FLAGS << SHIFTS[0]) | (ALL_FLAGS << SHIFTS[1]) |
              (ALL_FLAGS
               << SHIFTS[2]) | // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
              (ALL_FLAGS << SHIFTS[3]);
        }
    }
}

extern "C" void DMA1_Stream0_IRQHandler() { hal::stm32::step_interrupt(); }
extern "C" void DMA1_Stream1_IRQHandler() { hal::stm32::step_interrupt(); }
extern "C" void DMA1_Stream2_IRQHandler() { hal::stm32::step_interrupt(); }
extern "C" void DMA1_Stream3_IRQHandler() { hal::stm32::step_interrupt(); }
extern "C" void TIM7_IRQHandler()
{
    TIM7->SR = // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
      0U; // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    hal::stm32::step_interrupt();
}
