#pragma once

#include "hal/utilities/Result.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace hal
{
    class IEthernet
    {
      public:
        enum class EtherType : std::uint16_t
        {
            IPv4 = 0x0800U,
            ARP = 0x0806U,
            VLAN = 0x8100U,
            EtherCAT = 0x88A4U,
            SERVICE_VLAN = 0x88A8U,
            IPv6 = 0x86DDU
        };

        static constexpr std::size_t MAC_ADDRESS_SIZE{ 6U };
        using MacAddress = std::array<std::uint8_t, MAC_ADDRESS_SIZE>;

        static constexpr std::size_t ETHERNET_HEADER_SIZE{ 14U };
        static constexpr std::size_t VLAN_HEADER_SIZE{ 18U };
        static constexpr std::size_t MIN_FRAME_SIZE{ 60U };
        // Maximum raw frame without FCS: Ethernet header, two VLAN tags, and
        // the standard 1500-byte payload.
        static constexpr std::size_t MAX_FRAME_SIZE{ 1522U };
        static constexpr std::size_t ETHER_TYPE_OFFSET{ 12U };
        static constexpr std::size_t VLAN_TAG_SIZE{ 4U };
        static constexpr unsigned int BITS_PER_BYTE{ 8U };

        enum class Duplex : std::uint8_t
        {
            Unknown,
            Half,
            Full
        };

        struct LinkInfo
        {
            bool up{};
            std::uint32_t speed_mbps{};
            Duplex duplex{ Duplex::Unknown };
        };

        struct Configuration
        {
            std::string_view interface_name{ "eth0" };
            std::optional<EtherType> receive_ether_type{}; // No protocol filter unless requested.
            bool promiscuous{};
            std::uint8_t phy_address{};
        };

        virtual ~IEthernet() = default;

        IEthernet(const IEthernet&) = delete;
        IEthernet& operator=(const IEthernet&) = delete;
        IEthernet(IEthernet&&) = delete;
        IEthernet& operator=(IEthernet&&) = delete;

        // STM32 samples the negotiated PHY speed/duplex when starting. After
        // attaching a cable or renegotiating the link, stop and start again to
        // apply the new mode to the MAC.
        [[nodiscard]] virtual auto start() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto stop() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto isRunning() const noexcept -> bool = 0;

        [[nodiscard]] virtual auto getMacAddress() const noexcept -> MacAddress = 0;
        [[nodiscard]] virtual auto getLinkInfo() const noexcept -> util::Result<LinkInfo> = 0;

        [[nodiscard]] virtual auto transmit(std::span<const std::byte> frame,
                                            std::chrono::milliseconds timeout) noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto receive(std::span<std::byte> frame,
                                           std::chrono::milliseconds timeout) noexcept
          -> util::Result<std::size_t> = 0;

        [[nodiscard]] static constexpr auto frameEtherType(std::span<const std::byte> frame) noexcept
          -> std::optional<EtherType>
        {
            auto type{ frameOuterEtherType(frame) };
            if (!type) {
                return std::nullopt;
            }

            const auto read_ether_type = [&](std::size_t offset) {
                return static_cast<EtherType>(
                  static_cast<std::underlying_type_t<EtherType>>(std::to_integer<std::uint8_t>(frame[offset]))
                    << BITS_PER_BYTE |
                  std::to_integer<std::uint8_t>(frame[offset + 1U]));
            };

            std::size_t offset{ ETHER_TYPE_OFFSET };
            while (*type == EtherType::VLAN || *type == EtherType::SERVICE_VLAN) {
                offset += VLAN_TAG_SIZE;
                if (frame.size() < offset + sizeof(EtherType)) {
                    return std::nullopt;
                }
                type = read_ether_type(offset);
            }
            return type;
        }

        [[nodiscard]] static constexpr auto frameOuterEtherType(std::span<const std::byte> frame) noexcept
          -> std::optional<EtherType>
        {
            if (frame.size() < ETHERNET_HEADER_SIZE) {
                return std::nullopt;
            }
            return static_cast<EtherType>(static_cast<std::underlying_type_t<EtherType>>(
                                            std::to_integer<std::uint8_t>(frame[ETHER_TYPE_OFFSET]))
                                            << BITS_PER_BYTE |
                                          std::to_integer<std::uint8_t>(frame[ETHER_TYPE_OFFSET + 1U]));
        }

      protected:
        IEthernet() = default;
    };
}
