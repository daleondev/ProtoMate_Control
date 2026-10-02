#pragma once

#include "hal/drivers/itf/IEthernet.hpp"
#include "hal/hal.hpp"

#include <tx_api.h>

namespace hal
{
    class Ethernet final : public IEthernet
    {
      public:
        struct HardwareConfiguration
        {
            ETH_HandleTypeDef& handle;
            Configuration configuration;
        };

        explicit Ethernet(HardwareConfiguration configuration);
        ~Ethernet() override;

        Ethernet(const Ethernet&) = delete;
        Ethernet& operator=(const Ethernet&) = delete;
        Ethernet(Ethernet&&) = delete;
        Ethernet& operator=(Ethernet&&) = delete;

        [[nodiscard]] auto start() noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;

        [[nodiscard]] auto getMacAddress() const noexcept -> MacAddress override;
        [[nodiscard]] auto getLinkInfo() const noexcept -> util::Result<LinkInfo> override;

        [[nodiscard]] auto matchesConfiguration(const Configuration& configuration) const noexcept -> bool;

        [[nodiscard]] auto transmit(std::span<const std::byte> frame,
                                    std::chrono::milliseconds timeout) noexcept -> util::Result<> override;
        [[nodiscard]] auto receive(std::span<std::byte> frame, std::chrono::milliseconds timeout) noexcept
          -> util::Result<std::size_t> override;

      private:
        [[nodiscard]] auto waitForTransmit(std::uint32_t started_at, std::uint32_t timeout) noexcept
          -> util::Result<>;

        ETH_HandleTypeDef& m_handle;
        std::optional<EtherType> m_receiveEtherType;
        std::uint8_t m_phyAddress;
        bool m_promiscuous;
        bool m_running{};
        ETH_DMADescTypeDef* m_pendingTransmitDescriptor{};
        mutable TX_MUTEX m_stateMutex{};
        mutable TX_MUTEX m_phyMutex{};
        TX_MUTEX m_receiveMutex{};
        TX_MUTEX m_transmitMutex{};
    };
}
