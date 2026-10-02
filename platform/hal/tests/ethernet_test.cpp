#include "hal/drivers/factory/ethernet.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>

using namespace std::chrono_literals;

TEST(HalEthernet, ParsesRawAndVlanEtherTypes)
{
    std::array<std::byte, hal::IEthernet::VLAN_HEADER_SIZE> frame{};
    frame[12] = std::byte{ 0x88 };
    frame[13] = std::byte{ 0xA4 };
    EXPECT_EQ(hal::IEthernet::frameEtherType(frame), hal::IEthernet::EtherType::EtherCAT);

    frame[12] = std::byte{ 0x81 };
    frame[13] = std::byte{ 0x00 };
    frame[16] = std::byte{ 0x88 };
    frame[17] = std::byte{ 0xA4 };
    EXPECT_EQ(hal::IEthernet::frameOuterEtherType(frame), hal::IEthernet::EtherType::VLAN);
    EXPECT_EQ(hal::IEthernet::frameEtherType(frame), hal::IEthernet::EtherType::EtherCAT);

    std::array<std::byte, 22U> stacked_vlan_frame{};
    stacked_vlan_frame[12] = std::byte{ 0x88 };
    stacked_vlan_frame[13] = std::byte{ 0xA8 };
    stacked_vlan_frame[16] = std::byte{ 0x81 };
    stacked_vlan_frame[17] = std::byte{ 0x00 };
    stacked_vlan_frame[20] = std::byte{ 0x88 };
    stacked_vlan_frame[21] = std::byte{ 0xA4 };
    EXPECT_EQ(hal::IEthernet::frameEtherType(stacked_vlan_frame), hal::IEthernet::EtherType::EtherCAT);

    EXPECT_EQ(hal::IEthernet::frameEtherType(
                std::span<const std::byte>{ frame.data(), hal::IEthernet::ETHERNET_HEADER_SIZE }),
              std::nullopt);
}

TEST(HalEthernet, LinuxBackendValidatesStateWithoutRawSocketPrivileges)
{
    const auto ethernet{ hal::ethernet::create(hal::IEthernet::Configuration{ .interface_name = "lo" }) };
    ASSERT_NE(ethernet, nullptr);
    EXPECT_FALSE(ethernet->isRunning());
    EXPECT_TRUE(ethernet->stop().has_value());

    std::array<std::byte, hal::IEthernet::ETHERNET_HEADER_SIZE> frame{};
    const auto transmit{ ethernet->transmit(frame, 0ms) };
    ASSERT_FALSE(transmit.has_value());
    EXPECT_EQ(transmit.error(), std::make_error_code(std::errc::not_connected));

    const auto receive{ ethernet->receive(frame, 0ms) };
    ASSERT_FALSE(receive.has_value());
    EXPECT_EQ(receive.error(), std::make_error_code(std::errc::not_connected));

    const auto link{ ethernet->getLinkInfo() };
    if (!link &&
        (link.error() == std::make_error_code(std::errc::operation_not_permitted) ||
         link.error() == std::make_error_code(std::errc::permission_denied))) {
        GTEST_SKIP() << "network control sockets are unavailable";
    }
    ASSERT_TRUE(link.has_value());
    EXPECT_TRUE(link->up);
}

TEST(HalEthernet, RejectsInvalidInterfaceNameBeforeOpeningRawSocket)
{
    const std::string interface_name(128U, 'x');
    const auto ethernet{ hal::ethernet::create(
      hal::IEthernet::Configuration{ .interface_name = interface_name }) };
    ASSERT_NE(ethernet, nullptr);

    const auto result{ ethernet->start() };
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(HalEthernet, ValidatesFramesAndTimeoutsBeforeUsingSocket)
{
    const auto ethernet{ hal::ethernet::create(hal::IEthernet::Configuration{ .interface_name = "lo" }) };
    ASSERT_NE(ethernet, nullptr);

    std::array<std::byte, hal::IEthernet::ETHERNET_HEADER_SIZE> frame{};
    const auto short_transmit{ ethernet->transmit(
      std::span<const std::byte>{ frame.data(), hal::IEthernet::ETHERNET_HEADER_SIZE - 1U }, 0ms) };
    ASSERT_FALSE(short_transmit.has_value());
    EXPECT_EQ(short_transmit.error(), std::make_error_code(std::errc::invalid_argument));

    const auto negative_transmit{ ethernet->transmit(frame, -1ms) };
    ASSERT_FALSE(negative_transmit.has_value());
    EXPECT_EQ(negative_transmit.error(), std::make_error_code(std::errc::invalid_argument));

    std::span<std::byte> empty_frame{};
    const auto empty_receive{ ethernet->receive(empty_frame, 0ms) };
    ASSERT_FALSE(empty_receive.has_value());
    EXPECT_EQ(empty_receive.error(), std::make_error_code(std::errc::invalid_argument));

    const auto negative_receive{ ethernet->receive(frame, -1ms) };
    ASSERT_FALSE(negative_receive.has_value());
    EXPECT_EQ(negative_receive.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(HalEthernet, OpensAndClosesLinuxPacketSocketWhenPermitted)
{
    const auto ethernet{ hal::ethernet::create(hal::IEthernet::Configuration{ .interface_name = "lo" }) };
    ASSERT_NE(ethernet, nullptr);

    const auto started{ ethernet->start() };
    if (!started &&
        (started.error() == std::make_error_code(std::errc::operation_not_permitted) ||
         started.error() == std::make_error_code(std::errc::permission_denied))) {
        GTEST_SKIP() << "CAP_NET_RAW is unavailable";
    }
    ASSERT_TRUE(started.has_value()) << started.error().message();
    EXPECT_TRUE(ethernet->isRunning());
    EXPECT_TRUE(ethernet->stop().has_value());
    EXPECT_FALSE(ethernet->isRunning());
}
