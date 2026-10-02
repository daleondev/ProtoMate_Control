#pragma once

#include "hal/drivers/itf/IEthernet.hpp"

#include <net/if.h>
#include <pthread.h>

#include <array>

namespace hal
{
    class Ethernet final : public IEthernet
    {
      public:
        explicit Ethernet(Configuration configuration) noexcept;
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

        [[nodiscard]] auto transmit(std::span<const std::byte> frame,
                                    std::chrono::milliseconds timeout) noexcept -> util::Result<> override;
        [[nodiscard]] auto receive(std::span<std::byte> frame, std::chrono::milliseconds timeout) noexcept
          -> util::Result<std::size_t> override;

      private:
        std::array<char, IFNAMSIZ> m_interfaceName{};
        std::optional<EtherType> m_receiveEtherType;
        bool m_promiscuous{};
        int m_socket{ -1 };
        unsigned int m_interfaceIndex{};
        MacAddress m_macAddress{};
        mutable pthread_mutex_t m_lifecycleMutex{};
        pthread_mutex_t m_receiveMutex{};
        pthread_mutex_t m_transmitMutex{};
    };
}
