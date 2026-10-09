#include "hal/stm32/HalResult.hpp"
#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/stm32/Spi.hpp"
#include "hal/drivers/impl/stm32/Uart.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "spi.h"
#include "usart.h"
#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    class Transport : public testing::Test
    {
      protected:
        void SetUp() override
        {
            fake::reset();
            usart2.ISR = 0;
        }
    };
}

TEST(ExclusiveInstances, ResourceStaysClaimedUntilDestructionCompletes)
{
    struct Gate { std::atomic_bool entered{}, release{}; } gate;
    struct Driver
    {
        Gate* gate;
        explicit Driver(Gate* gate) : gate{ gate } {}
        ~Driver()
        {
            if (!gate) return;
            gate->entered.store(true);
            gate->entered.notify_one();
            gate->release.wait(false);
        }
    };
    hal::util::ExclusiveInstances<2> instances;
    auto driver{ instances.create<Driver>(0, &gate) };
    std::jthread destroy{ [driver = std::move(driver)]() mutable { driver.reset(); } };
    gate.entered.wait(false);
    EXPECT_FALSE(instances.create<Driver>(0, nullptr));
    EXPECT_TRUE(instances.create<Driver>(1, nullptr));
    gate.release.store(true);
    gate.release.notify_one();
    destroy.join();
    EXPECT_TRUE(instances.create<Driver>(0, nullptr));
}

TEST(ExclusiveInstances, FailedConstructionReleasesTheClaim)
{
    struct Driver { Driver() { throw std::runtime_error("construction failed"); } };
    hal::util::ExclusiveInstances<1> instances;
    EXPECT_THROW(static_cast<void>(instances.create<Driver>(0)), std::runtime_error);
    auto driver{ instances.create<int>(0, 42) };
    ASSERT_TRUE(driver);
    EXPECT_EQ(*driver, 42);
    EXPECT_FALSE(instances.create<int>(0, 7));
    EXPECT_FALSE(instances.create<int>(1, 7));
}

TEST_F(Transport, SpiFactoryOwnsConfiguredPeripheralAndReleasesIt)
{
    EXPECT_FALSE(hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi4 }));
    auto bus = hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 });
    ASSERT_TRUE(std::dynamic_pointer_cast<hal::Spi>(bus));
    EXPECT_EQ(fake::spi_init, 1U);
    EXPECT_EQ(bus->clockFrequencyHz(), 937'500U);
    hspi5.Init.BaudRatePrescaler = 3U << SPI_CFG1_MBR_Pos;
    EXPECT_EQ(bus->clockFrequencyHz(), 7'500'000U);
    EXPECT_FALSE(hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 }));
    bus.reset();
    EXPECT_EQ(fake::spi_deinit, 1U);
    EXPECT_TRUE(hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 }));
    EXPECT_EQ(fake::spi_init, 2U);
}

TEST_F(Transport, SpiUsesInjectedHandleAndRecoversFailures)
{
    SPI_HandleTypeDef handle;
    unsigned recovered{};
    // Factory recovery is tested below; this instance proves no hidden hspi5 dependency.
    hal::Spi spi{ { handle, []() -> std::uint32_t { return 32'000'000U; }, [] {}, [] {
        ++fake::spi_reset;
    } } };
    std::array<std::uint8_t, 3> tx{ 0xA3, 0x19, 0xFF }, rx{};
    ASSERT_TRUE(spi.exchange(tx, rx, 25ms));
    EXPECT_EQ(rx, tx);
    EXPECT_EQ(fake::spi_handle, &handle);
    EXPECT_EQ(fake::spi_timeout, 25U);
    for (auto status : { HAL_TIMEOUT, HAL_ERROR, HAL_BUSY }) {
        fake::spi_result = status;
        EXPECT_EQ(spi.exchange(tx, rx, 25ms).error(),
                  hal::make_error_code(static_cast<hal::HalError>(status)));
        EXPECT_EQ(fake::spi_reset, ++recovered);
    }
    fake::spi_result = HAL_OK;
    EXPECT_TRUE(spi.exchange(tx, rx, 25ms));
}

TEST_F(Transport, SpiRejectsInvalidTransfersWithoutTouchingHardware)
{
    auto bus = hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 });
    std::array<std::uint8_t, 3> tx{}, rx{};
    EXPECT_FALSE(bus->exchange({}, {}, 25ms));
    EXPECT_FALSE(bus->exchange(tx, std::span{ rx }.first(1), 25ms));
    EXPECT_FALSE(bus->exchange(tx, rx, 0ms));
    EXPECT_FALSE(bus->exchange(tx, rx, std::chrono::milliseconds{ 0xFFFFFFFFULL }));
    fake::ipsr = 16;
    EXPECT_FALSE(bus->exchange(tx, rx, 25ms));
    EXPECT_EQ(fake::spi_transfers, 0U);
    fake::ipsr = 0;
    fake::spi_result = HAL_TIMEOUT;
    EXPECT_FALSE(bus->exchange(tx, rx, 25ms));
    EXPECT_EQ(fake::spi_reset, 1U);
    EXPECT_EQ(fake::spi_init, 2U);
}

TEST_F(Transport, UartConsumesEchoAndReplyWithinOneBudget)
{
    auto bus = hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2,
                                   .transport = { .local_echo = true, .settle_time = 1ms } });
    ASSERT_TRUE(std::dynamic_pointer_cast<hal::Uart>(bus));
    EXPECT_FALSE(hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2 }));
    const std::array<std::uint8_t, 3> request{ 0x31, 0x72, 0xA9 };
    std::array<std::uint8_t, 2> reply{};
    fake::receive_after_transmit = { 0x31, 0x72, 0xA9, 0xCC, 0xDD };
    ASSERT_TRUE(bus->exchange(request, reply, 20ms));
    EXPECT_EQ(fake::uart_handle, &huart2);
    EXPECT_EQ(reply, (std::array<std::uint8_t, 2>{ 0xCC, 0xDD }));
    EXPECT_EQ(fake::uart_timeouts, (std::vector<std::uint32_t>{ 19, 17, 15 }));
    bus.reset();
    EXPECT_EQ(fake::uart_deinit, 1U);
    EXPECT_TRUE(hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2 }));
}

TEST_F(Transport, UartRejectsWrongEchoAndMissingReply)
{
    auto bus =
      hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2, .transport = { .local_echo = true } });
    const std::array<std::uint8_t, 2> request{ 1, 2 };
    std::array<std::uint8_t, 1> reply{};
    fake::receive_after_transmit = { 1, 3, 42 };
    EXPECT_EQ(bus->exchange(request, reply, 20ms).error(), std::errc::protocol_error);
    fake::receive_after_transmit = { 1, 2 };
    EXPECT_EQ(bus->exchange(request, reply, 20ms).error(), hal::make_error_code(hal::HalError::Timeout));
    EXPECT_TRUE(bus->exchange(request, {}, 20ms));
    const std::array<std::uint8_t, 16> full_fifo{};
    EXPECT_EQ(bus->exchange(full_fifo, reply, 20ms).error(), std::errc::invalid_argument);
    huart2.FifoMode = 0;
    EXPECT_EQ(bus->exchange(request, reply, 20ms).error(), std::errc::operation_not_permitted);
}

TEST_F(Transport, UartSupportsNonEchoDevicesWithoutPacketSizeAssumptions)
{
    USART_TypeDef peripheral;
    UART_HandleTypeDef handle{ &peripheral };
    hal::Uart uart{ { handle, {}, [] {} } };
    const std::array<std::uint8_t, 48> request{};
    std::array<std::uint8_t, 2> reply{};
    fake::receive_after_transmit = { 17, 23 };
    ASSERT_TRUE(uart.exchange(request, reply, 20ms));
    EXPECT_EQ(fake::uart_handle, &handle);
    EXPECT_EQ(reply, (std::array<std::uint8_t, 2>{ 17, 23 }));
    EXPECT_EQ(fake::transmitted.size(), 48U);
    EXPECT_TRUE(uart.exchange(request, {}, 20ms));
    fake::ipsr = 16;
    EXPECT_EQ(uart.exchange(request, reply, 20ms).error(), std::errc::invalid_argument);
}

TEST_F(Transport, FactoryResolvesMultipleCubeMxPeripheralsIndependently)
{
    using SpiId = hal::spi::Peripheral;
    using UartId = hal::uart::Peripheral;
    auto spi1 = hal::spi::create({ .peripheral = SpiId::Spi1 });
    ASSERT_TRUE(spi1);
    EXPECT_EQ(fake::spi_handle, &hspi1);
    auto spi5 = hal::spi::create({ .peripheral = SpiId::Spi5 });
    ASSERT_TRUE(spi5);
    EXPECT_EQ(fake::spi_handle, &hspi5);
    EXPECT_EQ(spi1->clockFrequencyHz(), 15'000'000U);
    EXPECT_EQ(spi5->clockFrequencyHz(), 937'500U);
    EXPECT_FALSE(hal::spi::create({ .peripheral = SpiId::Spi1 }));
    auto uart1 = hal::uart::create({ .peripheral = UartId::Usart1 });
    auto uart2 = hal::uart::create({ .peripheral = UartId::Usart2 });
    ASSERT_TRUE(uart1);
    ASSERT_TRUE(uart2);
    EXPECT_FALSE(hal::uart::create({ .peripheral = UartId::Usart1 }));
    std::array<std::uint8_t, 2> tx{ 1, 2 }, rx{};
    ASSERT_TRUE(spi1->exchange(tx, rx, 20ms));
    EXPECT_EQ(fake::spi_handle, &hspi1);
    ASSERT_TRUE(spi5->exchange(tx, rx, 20ms));
    EXPECT_EQ(fake::spi_handle, &hspi5);
    ASSERT_TRUE(uart1->exchange(tx, {}, 20ms));
    EXPECT_EQ(fake::uart_handle, &huart1);
    ASSERT_TRUE(uart2->exchange(tx, {}, 20ms));
    EXPECT_EQ(fake::uart_handle, &huart2);
    fake::spi_result = HAL_TIMEOUT;
    EXPECT_FALSE(spi1->exchange(tx, rx, 20ms));
    EXPECT_EQ(fake::spi_handle, &hspi1); // Recovery also uses the selected peripheral.
    spi1.reset();
    uart1.reset();
    EXPECT_TRUE(hal::spi::create({ .peripheral = SpiId::Spi1 }));
    EXPECT_FALSE(hal::spi::create({ .peripheral = SpiId::Spi5 }));
    EXPECT_TRUE(hal::uart::create({ .peripheral = UartId::Usart1 }));
    EXPECT_FALSE(hal::uart::create({ .peripheral = UartId::Usart2 }));
}

TEST_F(Transport, InvalidUnconfiguredAndReservedSelectionsHaveNoSideEffects)
{
    EXPECT_FALSE(hal::spi::create({ .peripheral = static_cast<hal::spi::Peripheral>(255) }));
    EXPECT_FALSE(hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi6 }));
    EXPECT_FALSE(hal::uart::create({ .peripheral = static_cast<hal::uart::Peripheral>(255) }));
    EXPECT_FALSE(hal::uart::create({ .peripheral = hal::uart::Peripheral::Uart8 }));
    EXPECT_FALSE(hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart3 }));
    EXPECT_EQ(fake::spi_init, 0U);
    EXPECT_EQ(fake::uart_init, 0U);
}

TEST_F(Transport, WiderCubeMxFramesCannotOverwriteByteBuffers)
{
    auto spi = hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 });
    auto uart = hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2 });
    std::array<std::uint8_t, 2> tx{}, rx{};
    hspi5.Init.DataSize = 15; // 16-bit SPI frames are incompatible with ISpi.
    EXPECT_EQ(spi->exchange(tx, rx, 20ms).error(), std::errc::operation_not_supported);
    hspi5.Init.DataSize = SPI_DATASIZE_8BIT;
    huart2.Init.WordLength = UART_WORDLENGTH_9B;
    huart2.Init.Parity = UART_PARITY_NONE;
    EXPECT_EQ(uart->exchange(tx, rx, 20ms).error(), std::errc::operation_not_supported);
    huart2.Init.WordLength = 8;
    EXPECT_EQ(fake::spi_transfers, 0U);
    EXPECT_TRUE(fake::transmitted.empty());
}
