#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/impl/stm32/StepHardware.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/FaultShutdown.hpp"

#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <vector>

extern "C" void DMA1_Stream0_IRQHandler();
extern "C" void DMA1_Stream1_IRQHandler();
extern "C" void DMA1_Stream2_IRQHandler();
extern "C" void DMA1_Stream3_IRQHandler();
extern "C" void TIM7_IRQHandler();

namespace
{
    using namespace std::chrono_literals;
    using hal::step::State;
    class RegisterTest : public ::testing::Test
    {
      protected:
        std::shared_ptr<hal::detail::StepGenerator> engine;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> axes;
        std::array<unsigned, 4> lengths{};
        std::array<bool, 3> high{}, hold_dma{};
        std::array<std::vector<std::uint64_t>, 3> rising, falling;
        std::uint64_t elapsed{};
        bool irqs{ true };
        constexpr static std::array shifts{ 0U, 6U, 16U, 22U };
        void SetUp() override
        {
            dma.LISR = 0;
            dma_streams = {};
            primask = 0;
            timer.CR1 = 0;
            auto hardware{ hal::stm32::makeStepHardware() };
            engine = std::make_shared<hal::detail::StepGenerator>(std::move(hardware));
            hal::stm32::registerStepGenerator(engine.get());
            for (unsigned i = 0; i < 3; ++i)
                axes[i] = engine->output(static_cast<hal::step::Axis>(i));
        }
        auto compare(unsigned i) -> uint32_t&
        {
            if (i == 0)
                return timer.CCR1;
            if (i == 1)
                return timer.CCR3;
            return timer.CCR4;
        }
        void start()
        {
            ASSERT_TRUE(engine->start());
            for (const auto& axis : axes) {
                if (axis->status().state == State::Ready) {
                    ASSERT_TRUE(axis->start(10us));
                }
            }
            for (unsigned i = 0; i < 4; ++i)
                lengths[i] = dma_streams[i].NDTR;
        }
        void dmaTransfer(unsigned i)
        {
            auto& stream = dma_streams[i];
            if (!(stream.CR & DMA_SxCR_EN) || (i < 3 && hold_dma[i]))
                return;
            const unsigned offset = (stream.CR & DMA_SxCR_MINC) ? lengths[i] - stream.NDTR : 0;
            auto* source =
              reinterpret_cast<const uint32_t*>((stream.CR & DMA_SxCR_CT) ? stream.M1AR : stream.M0AR);
            *reinterpret_cast<volatile uint32_t*>(stream.PAR) = source[offset];
            --stream.NDTR;
            if (stream.NDTR == 0) {
                dma.LISR |= 0x20U << shifts[i];
                if (stream.CR & DMA_SxCR_DBM) {
                    stream.CR ^= DMA_SxCR_CT;
                    stream.NDTR = lengths[i];
                }
                else
                    stream.CR &= ~DMA_SxCR_EN;
            }
            else if (stream.NDTR == lengths[i] / 2U && (stream.CR & DMA_SxCR_HTIE)) {
                dma.LISR |= 0x10U << shifts[i];
            }
        }
        void advance(std::uint64_t ticks)
        {
            while (ticks && (timer.CR1 & TIM_CR1_CEN)) {
                auto distance = [](uint32_t target) -> std::uint64_t {
                    auto d = hal::detail::stepDistance(timer.CNT, target);
                    return d ? d : hal::detail::step_park;
                };
                auto next = (timer.DIER & TIM_DIER_CC2DE)
                              ? distance(timer.CCR2)
                              : std::uint64_t{ hal::detail::step_park } - timer.CNT;
                std::array<std::uint64_t, 3> events;
                for (unsigned i = 0; i < 3; ++i) {
                    events[i] = compare(i) == hal::detail::step_park
                                  ? std::uint64_t{ hal::detail::step_park } - timer.CNT
                                  : distance(compare(i));
                    next = std::min(next, events[i]);
                }
                const auto poll_tick{ (elapsed / 10'000U + 1U) * 10'000U };
                if (TIM7->CR1 & TIM_CR1_CEN)
                    next = std::min(next, poll_tick - elapsed);
                if (next > ticks) {
                    timer.CNT = hal::detail::stepAdd(timer.CNT, ticks);
                    elapsed += ticks;
                    return;
                }
                const bool guard = next == distance(timer.CCR2);
                ticks -= next;
                elapsed += next;
                timer.CNT = hal::detail::stepAdd(timer.CNT, next);
                constexpr std::array flag{ 2U, 8U, 16U }, enable{ 1U, 256U, 4096U },
                  request{ 512U, 2048U, 4096U };
                for (unsigned i = 0; i < 3; ++i)
                    if (events[i] == next) {
                        if (compare(i) != hal::detail::step_park && (timer.CCER & enable[i])) {
                            auto* gpio = i == 0U ? GPIOA : GPIOB;
                            const auto mask = i == 0U ? GPIO_PIN_0 : i == 1U ? GPIO_PIN_10 : GPIO_PIN_11;
                            high[i] = !(gpio->IDR & mask);
                            gpio->IDR = (gpio->IDR & ~mask) | (high[i] ? mask : 0U);
                            (high[i] ? rising[i] : falling[i]).push_back(elapsed);
                        }
                        timer.SR |= flag[i];
                        if (timer.DIER & request[i])
                            dmaTransfer(i);
                    }
                if (guard && (timer.DIER & TIM_DIER_CC2DE))
                    dmaTransfer(3);
                if (irqs && !primask) {
                    if (elapsed == poll_tick && irq_enabled[TIM7_IRQn] && (TIM7->CR1 & TIM_CR1_CEN))
                        TIM7_IRQHandler();
                    constexpr std::array handlers{ DMA1_Stream0_IRQHandler,
                                                   DMA1_Stream1_IRQHandler,
                                                   DMA1_Stream2_IRQHandler,
                                                   DMA1_Stream3_IRQHandler };
                    for (unsigned i = 0; i < 4; ++i)
                        if (irq_enabled[i] && (dma.LISR & (0x3DU << shifts[i])))
                            handlers[i]();
                }
            }
        }
    };
}

TEST_F(RegisterTest, ConfiguresRealAdapterForIndependentWordDmaAndHardwareGuard)
{
    for (auto& axis : axes)
        ASSERT_TRUE(axis->prepare({ 100us, 5us }, 3));
    start();
    EXPECT_EQ(timer.PSC, 23U);
    EXPECT_EQ(timer.ARR, 0xFFFFFFFEU);
    EXPECT_EQ(timer.CCMR1, TIM_OCMODE_TOGGLE);
    EXPECT_EQ(timer.CCMR2, TIM_OCMODE_TOGGLE | (TIM_OCMODE_TOGGLE << 8U));
    EXPECT_EQ(timer.CCER, TIM_CCER_CC1E | TIM_CCER_CC3E | TIM_CCER_CC4E);
    EXPECT_EQ(gpio_a.alternate[0], 1U);
    EXPECT_EQ(gpio_b.alternate[10], 1U);
    EXPECT_EQ(gpio_b.alternate[11], 1U);
    EXPECT_EQ(mux[0].CCR, 18U);
    EXPECT_EQ(mux[1].CCR, 20U);
    EXPECT_EQ(mux[2].CCR, 21U);
    EXPECT_EQ(mux[3].CCR, 19U);
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_TRUE(dma_streams[i].CR & DMA_SxCR_DBM);
        EXPECT_TRUE(dma_streams[i].CR & DMA_SxCR_MINC);
    }
    EXPECT_EQ(dma_streams[3].PAR, reinterpret_cast<std::uintptr_t>(&timer.CR1));
    EXPECT_EQ(dma_streams[3].NDTR, 1U);
    EXPECT_FALSE(dma_streams[3].CR & DMA_SxCR_DBM);
}

TEST_F(RegisterTest, FatalShutdownStopsActiveDmaMotionWithoutDriverOrInterruptService)
{
    for (auto& axis : axes)
        ASSERT_TRUE(axis->prepare({ 10us, 5us }));
    start();
    advance(100U); // All three outputs are in their first high phase.
    for (const auto& edges : rising)
        ASSERT_EQ(edges.size(), 1U);
    gpio_e.level = 0U; // Drivers enabled.
    gpio_e.MODER = 0xAAAAAAAAU;
    gpio_e.OTYPER = 0xFFFFU;
    gpio_e.OSPEEDR = gpio_e.PUPDR = 0xFFFFFFFFU;
    hal::stm32::shutdownMotionOnFault();
    EXPECT_EQ(primask, 1U);
    EXPECT_EQ(timer.CR1, 0U);
    EXPECT_EQ(timer.DIER, 0U);
    EXPECT_EQ(timer.CCER, 0U);
    EXPECT_EQ(gpio_e.level & STEPPERS_EN_N_Pin, STEPPERS_EN_N_Pin);
    EXPECT_EQ(gpio_e.MODER >> 30U, 1U);
    EXPECT_EQ(gpio_e.MODER & 0x3FFFFFFFU, 0x2AAAAAAAU); // Other pins unchanged.
    EXPECT_EQ(gpio_e.OTYPER & STEPPERS_EN_N_Pin, 0U);
    EXPECT_EQ(gpio_e.OSPEEDR >> 30U, 0U);
    EXPECT_EQ(gpio_e.PUPDR >> 30U, 0U);
    EXPECT_EQ(gpio_a.level & M1_STEP_Pin, 0U);
    EXPECT_EQ(gpio_b.level & (M2_STEP_Pin | M3_STEP_Pin), 0U);
    EXPECT_EQ(gpio_a.MODER & 3U, 1U);
    EXPECT_EQ((gpio_b.MODER >> 20U) & 15U, 5U);
    // Even an already pending DMA request cannot restart output generation.
    for (unsigned i = 0; i < 4; ++i)
        dmaTransfer(i);
    advance(hal::detail::step_park * 2ULL);
    for (const auto& edges : rising)
        EXPECT_EQ(edges.size(), 1U);
}

TEST_F(RegisterTest, FatalShutdownBeforeInitializationEnablesGpioClocksAndIsIdempotent)
{
    tim2_clock_enabled = false;
    gpio_a_clock_enabled = gpio_b_clock_enabled = gpio_e_clock_enabled = false;
    timer.CR1 = 0xABCDU; // A clock-gated peripheral must not be accessed.
    gpio_a.level = M1_STEP_Pin;
    gpio_b.level = M2_STEP_Pin | M3_STEP_Pin;
    gpio_e.level = 0U;
    hal::stm32::shutdownMotionOnFault();
    hal::stm32::shutdownMotionOnFault();
    EXPECT_EQ(timer.CR1, 0xABCDU);
    EXPECT_FALSE(tim2_clock_enabled);
    EXPECT_TRUE(gpio_a_clock_enabled);
    EXPECT_TRUE(gpio_b_clock_enabled);
    EXPECT_TRUE(gpio_e_clock_enabled);
    EXPECT_EQ(gpio_e.level & STEPPERS_EN_N_Pin, STEPPERS_EN_N_Pin);
    EXPECT_EQ(gpio_a.level & M1_STEP_Pin, 0U);
    EXPECT_EQ(gpio_b.level & (M2_STEP_Pin | M3_STEP_Pin), 0U);
    timer.CR1 = 0U;
}

TEST_F(RegisterTest, ThreeRealDmaStreamsCompleteAtDifferentRatesWithoutGaps)
{
    for (unsigned i = 0; i < 3; ++i)
        ASSERT_TRUE(axes[i]->prepare({ std::chrono::microseconds{ 10 + 10 * i }, 5us }, 1100 - i * 100));
    start();
    advance(1'000'000U);
    EXPECT_EQ(engine->status().state, State::Running);
    EXPECT_EQ(axes[0]->status().state, State::Completed);
    for (unsigned i = 0; i < 3; ++i) {
        ASSERT_EQ(rising[i].size(), 1100 - i * 100);
        ASSERT_EQ(falling[i].size(), rising[i].size());
        EXPECT_EQ(engine->status().pulses[i], rising[i].size());
        for (unsigned p = 0; p < rising[i].size(); ++p) {
            EXPECT_EQ(falling[i][p] - rising[i][p], 50U);
            if (p) {
                EXPECT_EQ(rising[i][p] - rising[i][p - 1], 100U + 100U * i);
            }
        }
    }
    EXPECT_EQ(gpio_a.mode[0], GPIO_MODE_OUTPUT_PP);
    EXPECT_EQ(gpio_b.mode[10], GPIO_MODE_OUTPUT_PP);
}

TEST_F(RegisterTest, NormalFiniteGuardDoesNotNeedAnyIrqToPreventAnotherPulse)
{
    ASSERT_TRUE(axes[0]->prepare({ 10us, 5us }, 3));
    start();
    irqs = false;
    advance(hal::detail::step_park * 2ULL);
    EXPECT_FALSE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_EQ(rising[0].size(), 3U);
    EXPECT_EQ(falling[0].size(), 3U);
    EXPECT_EQ(engine->status().state, State::Underrun);
    EXPECT_EQ(axes[0]->status().state, State::Completed);
}

TEST_F(RegisterTest, WithheldRefillInterruptsStopCounterAtTheBufferHorizon)
{
    ASSERT_TRUE(axes[0]->prepare({ 10us, 5us }));
    start();
    irqs = false;
    advance(hal::detail::step_park * 2ULL);
    EXPECT_FALSE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_EQ(rising[0].size(), 512U);
    EXPECT_EQ(engine->status().state, State::Underrun);
    EXPECT_EQ(engine->status().pulses[0], 512U);
    DMA1_Stream0_IRQHandler();
    EXPECT_EQ(dma.LISR, 0U);
}

TEST_F(RegisterTest, TerminalPendingDmaIsCountedAndItsCompareIsParked)
{
    ASSERT_TRUE(axes[0]->prepare({ 100us, 5us }, 1));
    ASSERT_TRUE(axes[1]->prepare({ 17s, 5us }, 30));
    start();
    advance(100U);
    hold_dma[0] = true;
    advance(50U);
    EXPECT_EQ(axes[0]->pulseCount(), 1U);
    engine->service();
    EXPECT_EQ(timer.CCR1, hal::detail::step_park);
    advance(6'000'000'000ULL);
    EXPECT_EQ(rising[0].size(), 1U);
    EXPECT_EQ(rising[1].size(), 30U);
    EXPECT_EQ(engine->status().state, State::Running);
    EXPECT_EQ(axes[0]->status().state, State::Completed);
}

TEST_F(RegisterTest, DmaErrorIsLatchedAndDisablesAllOutputs)
{
    ASSERT_TRUE(axes[0]->prepare({ 100us, 5us }));
    start();
    advance(100U);
    dma.LISR |= 8U;
    DMA1_Stream0_IRQHandler();
    EXPECT_FALSE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_EQ(timer.CCER, 0U);
    EXPECT_EQ(engine->status().state, State::DmaError);
    EXPECT_FALSE(engine->status().counts_exact);
    EXPECT_EQ(gpio_a.mode[0], GPIO_MODE_OUTPUT_PP);
    EXPECT_EQ(gpio_a.level & 1U, 0U);
}

TEST_F(RegisterTest, LastAxisOwnerStopsHardwareAndDisconnectsPendingInterrupts)
{
    ASSERT_TRUE(axes[0]->prepare({ 100us, 5us }));
    start();
    advance(100U);
    engine.reset();
    EXPECT_TRUE(timer.CR1 & TIM_CR1_CEN);
    dma.LISR |= 0x20U;
    axes = {};
    EXPECT_FALSE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_EQ(timer.CCER, 0U);
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_FALSE(dma_streams[i].CR & DMA_SxCR_EN);
        EXPECT_FALSE(irq_enabled[i]);
    }
    // Even a dispatched stale vector must no longer reach the destroyed owner.
    DMA1_Stream0_IRQHandler();
    DMA1_Stream3_IRQHandler();
    EXPECT_EQ(dma.LISR, 0U);
}

TEST_F(RegisterTest, AbortingPartialDmaBuffersDoesNotCountShutdownTcifAsPulses)
{
    for (unsigned i = 0; i < 3U; ++i) {
        ASSERT_TRUE(axes[i]->prepare({ std::chrono::microseconds{ 100U << i }, 5us }));
    }
    start();
    advance(10'000U);
    const auto stopped{ engine->stop() };
    EXPECT_EQ(stopped.state, State::Stopped);
    EXPECT_TRUE(stopped.counts_exact);
    for (unsigned i = 0; i < 3U; ++i) {
        EXPECT_EQ(stopped.pulses[i], rising[i].size());
    }
}

TEST_F(RegisterTest, UnderrunDoesNotInventBlocksOnTheSlowerAxesDuringShutdown)
{
    for (unsigned i = 0; i < 3U; ++i) {
        ASSERT_TRUE(axes[i]->prepare({ std::chrono::microseconds{ 10U << i }, 5us }));
    }
    start();
    irqs = false;
    advance(200'000U);
    EXPECT_FALSE(timer.CR1 & TIM_CR1_CEN);
    const auto stopped{ engine->status() };
    EXPECT_EQ(stopped.state, State::Underrun);
    EXPECT_TRUE(stopped.counts_exact);
    for (unsigned i = 0; i < 3U; ++i) {
        EXPECT_EQ(stopped.pulses[i], rising[i].size());
        EXPECT_EQ(stopped.pulses[i], 512U >> i);
    }
}

TEST_F(RegisterTest, AxisStopAndRestartLeaveOtherChannelDmaAndPhaseUntouched)
{
    ASSERT_TRUE(axes[0]->prepare({ 10us, 5us }, 3000));
    start();
    advance(5000U);
    ASSERT_TRUE(axes[1]->prepare({ 20us, 5us }));
    ASSERT_TRUE(axes[1]->start(10us));
    lengths[1] = dma_streams[1].NDTR;
    advance(225U); // Stop M2 during a high phase, with a partial DMA buffer.
    const auto before{ timer.CNT };
    const auto stopped{ axes[1]->stop() };
    EXPECT_EQ(stopped.pulses, rising[1].size());
    EXPECT_EQ(timer.CNT, before);
    EXPECT_TRUE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_TRUE(timer.CCER & TIM_CCER_CC1E);
    EXPECT_TRUE(dma_streams[0].CR & DMA_SxCR_EN);
    EXPECT_FALSE(timer.CCER & TIM_CCER_CC3E);
    EXPECT_EQ(gpio_b.IDR & GPIO_PIN_10, 0U);
    ASSERT_TRUE(axes[1]->prepare({ 30us, 5us }, 10));
    ASSERT_TRUE(axes[1]->start(10us));
    lengths[1] = dma_streams[1].NDTR;
    advance(500'000U);
    EXPECT_EQ(axes[1]->pulseCount(), 10U);
    EXPECT_EQ(axes[0]->pulseCount(), 3000U);
    ASSERT_EQ(rising[0].size(), 3000U);
    for (std::size_t i = 1; i < rising[0].size(); ++i)
        EXPECT_EQ(rising[0][i] - rising[0][i - 1], 100U);
    EXPECT_TRUE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_EQ(timer.DIER & TIM_DIER_CC2DE, 0U); // No guard request at idle wrap.
}

TEST_F(RegisterTest, RuntimeTimingUpdatePreservesCountsAndOtherAxisWaveform)
{
    ASSERT_TRUE(axes[0]->prepare({ 20us, 5us }, 1600));
    ASSERT_TRUE(axes[1]->prepare({ 10us, 5us }, 3000));
    start();
    advance(10'000U);
    const auto update{ axes[0]->updateTiming({ 10us, 5us }) };
    ASSERT_TRUE(update);
    advance(500'000U);
    ASSERT_EQ(rising[0].size(), 1600U);
    ASSERT_EQ(rising[1].size(), 3000U);
    EXPECT_EQ(axes[0]->pulseCount(), 1600U);
    for (std::size_t i = 1; i < rising[0].size(); ++i) {
        EXPECT_EQ(rising[0][i] - rising[0][i - 1], i >= *update ? 100U : 200U);
    }
    for (std::size_t i = 1; i < rising[1].size(); ++i)
        EXPECT_EQ(rising[1][i] - rising[1][i - 1], 100U);
}

TEST_F(RegisterTest, IdleWrapDoesNotProducePulsesAndFiniteCompletionLeavesClockRunning)
{
    ASSERT_TRUE(engine->start());
    advance(hal::detail::step_park * 2ULL);
    EXPECT_TRUE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_TRUE(rising[0].empty());
    EXPECT_EQ(timer.DIER & TIM_DIER_CC2DE, 0U);
    ASSERT_TRUE(axes[0]->prepare({ 10us, 5us }, 1));
    ASSERT_TRUE(axes[0]->start(10us));
    lengths[0] = dma_streams[0].NDTR;
    lengths[3] = dma_streams[3].NDTR;
    advance(50'000U);
    EXPECT_EQ(rising[0].size(), 1U);
    EXPECT_EQ(axes[0]->status().state, State::Completed);
    EXPECT_TRUE(timer.CR1 & TIM_CR1_CEN);
    EXPECT_EQ(TIM7->CR1, 0U);
}
