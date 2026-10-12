#include "hal/drivers/impl/stm32/QuadratureEncoder.hpp"
#include <gtest/gtest.h>
#include <thread>

extern "C" void TIM3_IRQHandler();
extern "C" void TIM6_DAC_IRQHandler();

namespace
{
    using namespace std::chrono_literals;
    class EncoderRegisterTest : public ::testing::Test
    {
      protected:
        std::unique_ptr<hal::QuadratureEncoder> encoder;
        void SetUp() override
        {
            primask = 0U;
            encoder = std::make_unique<hal::QuadratureEncoder>(
                hal::util::TimerLease{hal::timer::Peripheral::Tim3},
                hal::util::TimerLease{hal::timer::Peripheral::Tim6}, nullptr, nullptr);
        }
    };

    TEST_F(EncoderRegisterTest, SamplingInterruptPublishesWithoutPositionPolling)
    {
        unsigned callbacks{};
        hal::IQuadratureEncoder::Count position{};
        encoder->setSampleCallback([&](const auto& sample) noexcept {
            ++callbacks;
            if (sample.position) { position = *sample.position; }
        });
        EXPECT_FALSE(encoder->isRunning());
        EXPECT_EQ(TIM6->CR1 & TIM_CR1_CEN, 0U);
        ASSERT_TRUE(encoder->start());
        EXPECT_TRUE(irq_enabled[TIM6_DAC_IRQn]);
        const auto initial = callbacks;
        TIM3->CNT = 100U;
        std::this_thread::sleep_for(12ms);
        TIM6->SR = TIM_SR_UIF;
        TIM6_DAC_IRQHandler();
        EXPECT_EQ(callbacks, initial + 1U);
        EXPECT_EQ(position, 100);
        EXPECT_EQ(TIM6->SR & TIM_SR_UIF, 0U);
        ASSERT_TRUE(encoder->stop());
        EXPECT_FALSE(irq_enabled[TIM6_DAC_IRQn]);
        EXPECT_EQ(TIM6->CR1 & TIM_CR1_CEN, 0U);
        const auto stopped = callbacks;
        TIM6->SR = TIM_SR_UIF;
        TIM6_DAC_IRQHandler();
        EXPECT_EQ(callbacks, stopped);
        encoder->clearSampleCallback();
    }

    TEST_F(EncoderRegisterTest, WrapInterruptsPreserveSignedPositionAcrossRestart)
    {
        ASSERT_TRUE(encoder->start());
        for (auto count : {0x5555U, 0xAAAAU, 0U}) {
            TIM3->CNT = count;
            TIM3->SR = TIM_FLAG_UPDATE | TIM_FLAG_CC3 | TIM_FLAG_CC4;
            TIM3_IRQHandler();
        }
        EXPECT_EQ(encoder->position(), 65536);
        EXPECT_FALSE(encoder->setPosition(0));
        ASSERT_TRUE(encoder->stop());
        ASSERT_TRUE(encoder->start());
        TIM3->CNT = 65535U;
        TIM3->SR = TIM_FLAG_UPDATE;
        TIM3_IRQHandler();
        EXPECT_EQ(encoder->position(), 65535);
    }

    TEST_F(EncoderRegisterTest, DestructionReleasesBothTimersAndDisablesInterrupts)
    {
        ASSERT_TRUE(encoder->start());
        EXPECT_FALSE(hal::util::TimerLease{hal::timer::Peripheral::Tim3});
        EXPECT_FALSE(hal::util::TimerLease{hal::timer::Peripheral::Tim6});
        encoder.reset();
        EXPECT_FALSE(irq_enabled[TIM3_IRQn]);
        EXPECT_FALSE(irq_enabled[TIM6_DAC_IRQn]);
        EXPECT_TRUE(hal::util::TimerLease{hal::timer::Peripheral::Tim3});
        EXPECT_TRUE(hal::util::TimerLease{hal::timer::Peripheral::Tim6});
    }
}
