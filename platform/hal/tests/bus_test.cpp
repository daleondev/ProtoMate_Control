#include "hal/board/board.hpp"
#include "hal/devices/factory/tmc2209.hpp"
#include "hal/devices/impl/Tmc2209.hpp"
#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/linux/Spi.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
}

TEST(HalSpi, FactoryAndBoardShareExclusivePeripheralOwnership)
{
    EXPECT_FALSE(hal::spi::create({ .peripheral = static_cast<hal::spi::Peripheral>(255) }));
    auto independent{ hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi6 }) };
    ASSERT_TRUE(independent);
    auto bus{ hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 }) };
    ASSERT_TRUE(std::dynamic_pointer_cast<hal::Spi>(bus));
    EXPECT_EQ(bus->clockFrequencyHz(), 937'500U);
    EXPECT_FALSE(hal::board::createEthercatSpi());
    auto alias{ bus };
    bus.reset();
    EXPECT_FALSE(hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 }));
    alias.reset();
    EXPECT_TRUE(hal::board::createEthercatSpi());
}

TEST(HalSpi, ModelRequiresAnAttachedPeerAndHasNoDeviceProtocol)
{
    auto bus{ std::dynamic_pointer_cast<hal::Spi>(
      hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 })) };
    ASSERT_TRUE(bus);
    std::array<std::uint8_t, 37> tx{}, rx{};
    tx.fill(0xA6);
    EXPECT_EQ(bus->exchange(tx, rx, 20ms).error(), std::errc::not_connected);
    unsigned exchanges{};
    bus->setExchangeHandler([&](auto request, auto reply, auto timeout) -> hal::util::Result<> {
        ++exchanges;
        EXPECT_EQ(timeout, 20ms);
        std::transform(
          request.begin(), request.end(), reply.begin(), [](auto value) { return value ^ 0xFF; });
        return {};
    });
    ASSERT_TRUE(bus->exchange(tx, rx, 20ms));
    EXPECT_TRUE(std::ranges::all_of(rx, [](auto value) { return value == 0x59; }));
    EXPECT_EQ(bus->exchange({}, {}, 20ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(bus->exchange(tx, std::span{ rx }.first(1), 20ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(bus->exchange(tx, rx, 0ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(exchanges, 1U);
    bus->setExchangeHandler([](auto, auto, auto) -> hal::util::Result<> {
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    });
    EXPECT_EQ(bus->exchange(tx, rx, 20ms).error(), std::errc::timed_out);
}

TEST(HalUart, GenericFactoryIsIndependentOfAttachedStepperDevices)
{
    EXPECT_FALSE(hal::uart::create({ .peripheral = static_cast<hal::uart::Peripheral>(255) }));
    EXPECT_FALSE(hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart3 }));
    EXPECT_FALSE(hal::uart::create(
      { .peripheral = hal::uart::Peripheral::Usart2, .transport = { .settle_time = -1ms } }));
    auto bus{ hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2 }) };
    ASSERT_TRUE(std::dynamic_pointer_cast<hal::Uart>(bus));
    EXPECT_FALSE(hal::board::createStepperDriverBus());
    std::array<std::uint8_t, 4> request{ 5, 0, 0, 0x48 };
    std::array<std::uint8_t, 8> reply{};
    EXPECT_EQ(bus->exchange(request, reply, 20ms).error(), std::errc::not_connected);
    bus.reset();
    EXPECT_TRUE(hal::board::createStepperDriverBus());
}

TEST(HalUart, LinuxBoardOwnsFreshDeviceModelsForEachBusLifetime)
{
    for (unsigned lifetime{}; lifetime < 2; ++lifetime) {
        auto bus{ hal::board::createStepperDriverBus() };
        ASSERT_TRUE(bus);
        EXPECT_FALSE(hal::board::createStepperDriverBus());
        for (std::uint8_t address{}; address < 2; ++address) {
            hal::device::Tmc2209 driver{ bus, address };
            auto status{ driver.status() };
            ASSERT_TRUE(status);
            EXPECT_TRUE(status->reset());
            ASSERT_TRUE(driver.initialize({}));
            status = driver.status();
            ASSERT_TRUE(status);
            EXPECT_FALSE(status->reset());
        }
    }
}

TEST(HalUart, TmcTransportFactoryUsesTheRequestedPeripheral)
{
    auto occupied{ hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2 }) };
    ASSERT_TRUE(occupied);
    EXPECT_FALSE(hal::device::tmc2209::createTransport({ .peripheral = hal::uart::Peripheral::Usart2 }));
    auto transport{ hal::device::tmc2209::createTransport({ .peripheral = hal::uart::Peripheral::Usart1 }) };
    ASSERT_TRUE(std::dynamic_pointer_cast<hal::Uart>(transport));
    EXPECT_FALSE(hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart1 }));
    hal::device::Tmc2209 driver{ transport, 0 };
    ASSERT_TRUE(driver.initialize({}));
    EXPECT_TRUE(driver.verify());
}

TEST(HalUart, ArbitraryLengthRequestReplyAndWriteOnlyExchanges)
{
    auto bus{ std::dynamic_pointer_cast<hal::Uart>(
      hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2 })) };
    ASSERT_TRUE(bus);
    std::array<std::uint8_t, 64> request{};
    std::array<std::uint8_t, 32> reply{};
    unsigned exchanges{};
    bus->setExchangeHandler([&](auto tx, auto rx, auto timeout) -> hal::util::Result<> {
        ++exchanges;
        EXPECT_EQ(tx.size(), request.size());
        EXPECT_EQ(timeout, 20ms);
        std::ranges::fill(rx, 0xA5);
        return {};
    });
    ASSERT_TRUE(bus->exchange(request, reply, 20ms));
    EXPECT_TRUE(std::ranges::all_of(reply, [](auto value) { return value == 0xA5; }));
    EXPECT_TRUE(bus->exchange(request, {}, 20ms));
    EXPECT_EQ(bus->exchange({}, reply, 20ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(bus->exchange(request, reply, -1ms).error(), std::errc::invalid_argument);
    const std::vector<std::uint8_t> oversized(65'536);
    EXPECT_EQ(bus->exchange(oversized, {}, 20ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(exchanges, 2U);
}

TEST(HalUart, EchoConfigurationBoundsPollingFifoAndReservesSettleTime)
{
    auto bus{ std::dynamic_pointer_cast<hal::Uart>(
      hal::uart::create({ .peripheral = hal::uart::Peripheral::Usart2,
                          .transport = { .local_echo = true, .settle_time = 1ms } })) };
    ASSERT_TRUE(bus);
    std::array<std::uint8_t, 8> request{}, reply{};
    unsigned exchanges{};
    bus->setExchangeHandler([&](auto, auto, auto timeout) -> hal::util::Result<> {
        ++exchanges;
        EXPECT_EQ(timeout, 19ms);
        return {};
    });
    EXPECT_TRUE(bus->exchange(request, reply, 20ms));
    const std::array<std::uint8_t, 9> too_long{};
    EXPECT_EQ(bus->exchange(too_long, reply, 20ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(bus->exchange(request, reply, 1ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(exchanges, 1U);
}

TEST(HalBuses, DifferentPeripheralSelectionsHaveIndependentModels)
{
    using SpiId = hal::spi::Peripheral;
    using UartId = hal::uart::Peripheral;
    auto spi1 = std::dynamic_pointer_cast<hal::Spi>(hal::spi::create({ .peripheral = SpiId::Spi1 }));
    auto spi2 = std::dynamic_pointer_cast<hal::Spi>(hal::spi::create({ .peripheral = SpiId::Spi2 }));
    auto uart1 = std::dynamic_pointer_cast<hal::Uart>(hal::uart::create({ .peripheral = UartId::Usart1 }));
    auto uart2 = std::dynamic_pointer_cast<hal::Uart>(hal::uart::create({ .peripheral = UartId::Usart2 }));
    ASSERT_TRUE(spi1);
    ASSERT_TRUE(spi2);
    ASSERT_TRUE(uart1);
    ASSERT_TRUE(uart2);
    auto replyWith = [](std::uint8_t value) {
        return [value](auto, auto reply, auto) -> hal::util::Result<> {
            std::ranges::fill(reply, value);
            return {};
        };
    };
    spi1->setExchangeHandler(replyWith(1));
    spi2->setExchangeHandler(replyWith(2));
    uart1->setExchangeHandler(replyWith(3));
    uart2->setExchangeHandler(replyWith(4));
    std::array<std::uint8_t, 1> tx{}, rx{};
    ASSERT_TRUE(spi1->exchange(tx, rx, 20ms));
    EXPECT_EQ(rx[0], 1);
    ASSERT_TRUE(spi2->exchange(tx, rx, 20ms));
    EXPECT_EQ(rx[0], 2);
    ASSERT_TRUE(uart1->exchange(tx, rx, 20ms));
    EXPECT_EQ(rx[0], 3);
    ASSERT_TRUE(uart2->exchange(tx, rx, 20ms));
    EXPECT_EQ(rx[0], 4);
    spi1.reset();
    uart1.reset();
    EXPECT_TRUE(hal::spi::create({ .peripheral = SpiId::Spi1 }));
    EXPECT_FALSE(hal::spi::create({ .peripheral = SpiId::Spi2 }));
    EXPECT_TRUE(hal::uart::create({ .peripheral = UartId::Usart1 }));
    EXPECT_FALSE(hal::uart::create({ .peripheral = UartId::Usart2 }));
}
