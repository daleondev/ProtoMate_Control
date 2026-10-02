#include "Ethernet.hpp"

#include "ethernet_dma.h"
#include "hal/drivers/common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <system_error>

namespace
{
    constexpr std::size_t RX_SLOT_COUNT{ ETH_RX_DESC_CNT * 2U };
    constexpr std::uint32_t PHY_BASIC_STATUS_REGISTER{ 1U };
    constexpr std::uint32_t PHY_LINK_STATUS{ 1U << 2U };
    constexpr std::uint32_t PHY_SPECIAL_CONTROL_STATUS_REGISTER{ 31U };
    constexpr std::uint32_t PHY_SPEED_DUPLEX_MASK{ 0x001CU };
    constexpr std::uint32_t PHY_10_HALF_DUPLEX{ 0x0004U };
    constexpr std::uint32_t PHY_100_HALF_DUPLEX{ 0x0008U };
    constexpr std::uint32_t PHY_10_FULL_DUPLEX{ 0x0014U };
    constexpr std::uint32_t PHY_100_FULL_DUPLEX{ 0x0018U };

    struct RxSlot
    {
        ETH_BufferTypeDef link{};
        alignas(ETH_DMA_BUFFER_ALIGNMENT) std::array<std::byte, ETH_DMA_FRAME_BUFFER_SIZE> data{};
        bool allocated{};
    };

    std::array<RxSlot, RX_SLOT_COUNT> rx_slots ETH_DMA_BUFFER_ATTRIBUTE;
    std::array<std::byte, ETH_DMA_FRAME_BUFFER_SIZE> tx_frame ETH_DMA_BUFFER_ATTRIBUTE;

    CHAR state_mutex_name[] = "Ethernet state";
    CHAR phy_mutex_name[] = "Ethernet PHY";
    CHAR receive_mutex_name[] = "Ethernet receive";
    CHAR transmit_mutex_name[] = "Ethernet transmit";

    class ThreadXLock
    {
      public:
        explicit ThreadXLock(TX_MUTEX& mutex)
          : m_mutex{ mutex }
        {
            if (tx_mutex_get(&m_mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
                std::terminate();
            }
        }

        ~ThreadXLock()
        {
            if (tx_mutex_put(&m_mutex) != TX_SUCCESS) {
                std::terminate();
            }
        }

        ThreadXLock(const ThreadXLock&) = delete;
        ThreadXLock& operator=(const ThreadXLock&) = delete;
        ThreadXLock(ThreadXLock&&) = delete;
        ThreadXLock& operator=(ThreadXLock&&) = delete;

      private:
        TX_MUTEX& m_mutex;
    };

    template<typename T = void>
    [[nodiscard]] auto error_result(std::errc error) noexcept -> util::Result<T>
    {
        return std::unexpected(std::make_error_code(error));
    }

    [[nodiscard]] auto timeout_milliseconds(std::chrono::milliseconds timeout) noexcept -> std::uint32_t
    {
        if (timeout.count() <= 0) {
            return 0U;
        }
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(timeout.count()),
                                                                  std::numeric_limits<std::uint32_t>::max()));
    }

    [[nodiscard]] auto find_slot(const std::uint8_t* buffer) noexcept -> RxSlot*
    {
        const auto slot{ std::ranges::find_if(rx_slots, [&](const RxSlot& candidate) {
            return candidate.data.data() == reinterpret_cast<const std::byte*>(buffer);
        }) };
        return slot == rx_slots.end() ? nullptr : &*slot;
    }

    auto release_frame(ETH_BufferTypeDef* first) noexcept -> void
    {
        while (first != nullptr) {
            ETH_BufferTypeDef* const next{ first->next };
            const auto slot{ std::ranges::find_if(
              rx_slots, [&](const RxSlot& candidate) { return &candidate.link == first; }) };
            if (slot != rx_slots.end()) {
                slot->link = {};
                slot->allocated = false;
            }
            first = next;
        }
    }
}

extern "C" void HAL_ETH_RxAllocateCallback(std::uint8_t** buffer)
{
    if (buffer == nullptr) {
        return;
    }
    *buffer = nullptr;
    const auto slot{ std::ranges::find_if(rx_slots,
                                          [](const RxSlot& candidate) { return !candidate.allocated; }) };
    if (slot != rx_slots.end()) {
        slot->allocated = true;
        slot->link = {};
        *buffer = reinterpret_cast<std::uint8_t*>(slot->data.data());
    }
}

extern "C" void HAL_ETH_RxLinkCallback(void** start, void** end, std::uint8_t* buffer, std::uint16_t length)
{
    if (start == nullptr || end == nullptr) {
        return;
    }
    RxSlot* const slot{ find_slot(buffer) };
    if (slot == nullptr) {
        return;
    }

    slot->link.buffer = buffer;
    slot->link.len = length;
    slot->link.next = nullptr;
    if (*start == nullptr) {
        *start = &slot->link;
    }
    else {
        static_cast<ETH_BufferTypeDef*>(*end)->next = &slot->link;
    }
    *end = &slot->link;
}

namespace hal
{
    Ethernet::Ethernet(HardwareConfiguration configuration)
      : m_handle{ configuration.handle }
      , m_receiveEtherType{ configuration.configuration.receive_ether_type }
      , m_phyAddress{ configuration.configuration.phy_address }
      , m_promiscuous{ configuration.configuration.promiscuous }
    {
        for (auto& slot : rx_slots) {
            slot.link = {};
            slot.allocated = false;
        }
        if (tx_mutex_create(&m_stateMutex, state_mutex_name, TX_INHERIT) != TX_SUCCESS ||
            tx_mutex_create(&m_phyMutex, phy_mutex_name, TX_INHERIT) != TX_SUCCESS ||
            tx_mutex_create(&m_receiveMutex, receive_mutex_name, TX_INHERIT) != TX_SUCCESS ||
            tx_mutex_create(&m_transmitMutex, transmit_mutex_name, TX_INHERIT) != TX_SUCCESS) {
            std::terminate();
        }
    }

    Ethernet::~Ethernet()
    {
        static_cast<void>(stop());
        if (tx_mutex_delete(&m_transmitMutex) != TX_SUCCESS ||
            tx_mutex_delete(&m_receiveMutex) != TX_SUCCESS || tx_mutex_delete(&m_phyMutex) != TX_SUCCESS ||
            tx_mutex_delete(&m_stateMutex) != TX_SUCCESS) {
            std::terminate();
        }
    }

    auto Ethernet::start() noexcept -> util::Result<>
    {
        ThreadXLock transmit_lock{ m_transmitMutex };
        ThreadXLock receive_lock{ m_receiveMutex };
        ThreadXLock state_lock{ m_stateMutex };
        if (m_running) {
            return {};
        }

        const auto link{ getLinkInfo() };
        if (!link) {
            return std::unexpected(link.error());
        }

        ETH_MACConfigTypeDef mac_configuration{};
        if (HAL_ETH_GetMACConfig(&m_handle, &mac_configuration) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        mac_configuration.Speed = link->speed_mbps == 10U ? ETH_SPEED_10M : ETH_SPEED_100M;
        mac_configuration.DuplexMode =
          link->duplex == Duplex::Half ? ETH_HALFDUPLEX_MODE : ETH_FULLDUPLEX_MODE;
        mac_configuration.DropTCPIPChecksumErrorPacket = DISABLE;
        mac_configuration.TransmitFlowControl = DISABLE;
        mac_configuration.ReceiveFlowControl = DISABLE;
        // The public frame limit includes two VLAN tags but excludes the FCS.
        // The MAC's default size check only accommodates a single VLAN tag.
        mac_configuration.GiantPacketSizeLimitControl = ENABLE;
        mac_configuration.GiantPacketSizeLimit = MAX_FRAME_SIZE + 4U;
        if (HAL_ETH_SetMACConfig(&m_handle, &mac_configuration) != HAL_OK) {
            return error_result(std::errc::io_error);
        }

        ETH_MACFilterConfigTypeDef filter{};
        if (HAL_ETH_GetMACFilterConfig(&m_handle, &filter) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        filter.PromiscuousMode = m_promiscuous ? ENABLE : DISABLE;
        if (HAL_ETH_SetMACFilterConfig(&m_handle, &filter) != HAL_OK) {
            return error_result(std::errc::io_error);
        }

        if (HAL_ETH_Start(&m_handle) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        m_running = true;
        return {};
    }

    auto Ethernet::stop() noexcept -> util::Result<>
    {
        ThreadXLock transmit_lock{ m_transmitMutex };
        ThreadXLock receive_lock{ m_receiveMutex };
        ThreadXLock state_lock{ m_stateMutex };
        if (!m_running) {
            return {};
        }
        ThreadXLock phy_lock{ m_phyMutex };
        ETH_MACConfigTypeDef mac_configuration{};
        ETH_MACFilterConfigTypeDef filter{};
        if (HAL_ETH_GetMACConfig(&m_handle, &mac_configuration) != HAL_OK ||
            HAL_ETH_GetMACFilterConfig(&m_handle, &filter) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        if (HAL_ETH_Stop(&m_handle) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        m_running = false;

        // HAL_Start rebuilds the RX ring without rewinding the DMA cursor.
        // Queued frames can then leave its current descriptor equal to the
        // tail, permanently suspending reception. Reset the MAC/DMA together
        // with both software rings, also cancelling any timed-out TX packet.
        // Keep the PHY negotiated and preserve the MAC mode (including loopback).
        m_handle.TxDescList = {};
        m_handle.RxDescList = {};
        if (HAL_ETH_Init(&m_handle) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        m_pendingTransmitDescriptor = nullptr;
        for (auto& slot : rx_slots) {
            slot.link = {};
            slot.allocated = false;
        }
        if (HAL_ETH_SetMACConfig(&m_handle, &mac_configuration) != HAL_OK ||
            HAL_ETH_SetMACFilterConfig(&m_handle, &filter) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        return {};
    }

    auto Ethernet::isRunning() const noexcept -> bool
    {
        ThreadXLock state_lock{ m_stateMutex };
        return m_running;
    }

    auto Ethernet::getMacAddress() const noexcept -> MacAddress
    {
        MacAddress address{};
        if (m_handle.Init.MACAddr != nullptr) {
            std::copy_n(m_handle.Init.MACAddr, address.size(), address.begin());
        }
        return address;
    }

    auto Ethernet::getLinkInfo() const noexcept -> util::Result<LinkInfo>
    {
        if (m_phyAddress > 31U) {
            return error_result<LinkInfo>(std::errc::invalid_argument);
        }
        // The MDIO address/data registers form one transaction shared by all
        // callers. A concurrent read must not overwrite an unfinished one.
        ThreadXLock phy_lock{ m_phyMutex };
        std::uint32_t basic_status{};
        if (HAL_ETH_ReadPHYRegister(&m_handle, m_phyAddress, PHY_BASIC_STATUS_REGISTER, &basic_status) !=
              HAL_OK ||
            HAL_ETH_ReadPHYRegister(&m_handle, m_phyAddress, PHY_BASIC_STATUS_REGISTER, &basic_status) !=
              HAL_OK) {
            return error_result<LinkInfo>(std::errc::io_error);
        }

        LinkInfo info{ .up = (basic_status & PHY_LINK_STATUS) != 0U };
        if (!info.up) {
            return info;
        }

        std::uint32_t special_status{};
        if (HAL_ETH_ReadPHYRegister(
              &m_handle, m_phyAddress, PHY_SPECIAL_CONTROL_STATUS_REGISTER, &special_status) != HAL_OK) {
            return error_result<LinkInfo>(std::errc::io_error);
        }
        switch (special_status & PHY_SPEED_DUPLEX_MASK) {
            case PHY_10_HALF_DUPLEX:
                info.speed_mbps = 10U;
                info.duplex = Duplex::Half;
                break;
            case PHY_100_HALF_DUPLEX:
                info.speed_mbps = 100U;
                info.duplex = Duplex::Half;
                break;
            case PHY_10_FULL_DUPLEX:
                info.speed_mbps = 10U;
                info.duplex = Duplex::Full;
                break;
            case PHY_100_FULL_DUPLEX:
                info.speed_mbps = 100U;
                info.duplex = Duplex::Full;
                break;
            default:
                break;
        }
        return info;
    }

    auto Ethernet::matchesConfiguration(const Configuration& configuration) const noexcept -> bool
    {
        return m_receiveEtherType == configuration.receive_ether_type &&
               m_phyAddress == configuration.phy_address && m_promiscuous == configuration.promiscuous;
    }

    auto Ethernet::waitForTransmit(std::uint32_t started_at, std::uint32_t timeout) noexcept -> util::Result<>
    {
        if (m_pendingTransmitDescriptor == nullptr) {
            return {};
        }

        while ((m_pendingTransmitDescriptor->DESC3 & ETH_DMATXNDESCWBF_OWN) != 0U) {
            if ((m_handle.Instance->DMACSR & ETH_DMACSR_FBE) != 0U) {
                return error_result(std::errc::io_error);
            }
            if (timeout == 0U || HAL_GetTick() - started_at >= timeout) {
                // Keep both the descriptor and tx_frame owned by this packet.
                // A subsequent transmit must wait before overwriting the data.
                return error_result(std::errc::timed_out);
            }
        }

        __DMB();
        const bool failed{ (m_pendingTransmitDescriptor->DESC3 & ETH_DMATXNDESCWBF_ES) != 0U };
        static_cast<void>(HAL_ETH_ReleaseTxPacket(&m_handle));
        m_pendingTransmitDescriptor = nullptr;
        return failed ? error_result(std::errc::io_error) : util::Result<>{};
    }

    auto Ethernet::transmit(std::span<const std::byte> frame, std::chrono::milliseconds timeout) noexcept
      -> util::Result<>
    {
        if (frame.size() < ETHERNET_HEADER_SIZE || frame.size() > MAX_FRAME_SIZE || timeout.count() < 0) {
            return error_result(std::errc::invalid_argument);
        }

        ThreadXLock transmit_lock{ m_transmitMutex };
        if (!isRunning()) {
            return error_result(std::errc::not_connected);
        }

        const std::uint32_t started_at{ HAL_GetTick() };
        const std::uint32_t timeout_ticks{ timeout_milliseconds(timeout) };
        const auto previous_transmission{ waitForTransmit(started_at, timeout_ticks) };
        if (!previous_transmission) {
            return previous_transmission;
        }

        std::ranges::copy(frame, tx_frame.begin());
        const MacAddress source{ getMacAddress() };
        for (std::size_t index{}; index < source.size(); ++index) {
            tx_frame[MAC_ADDRESS_SIZE + index] = static_cast<std::byte>(source[index]);
        }

        ETH_BufferTypeDef buffer{ .buffer = reinterpret_cast<std::uint8_t*>(tx_frame.data()),
                                  .len = static_cast<std::uint32_t>(frame.size()),
                                  .next = nullptr };
        ETH_TxPacketConfigTypeDef packet{};
        packet.Attributes = ETH_TX_PACKETS_FEATURES_SAIC | ETH_TX_PACKETS_FEATURES_CRCPAD;
        packet.Length = static_cast<std::uint32_t>(frame.size());
        packet.TxBuffer = &buffer;
        packet.SrcAddrCtrl = ETH_SRC_ADDR_REPLACE;
        packet.CRCPadCtrl = ETH_CRC_PAD_INSERT;
        packet.pData = tx_frame.data();
        const auto descriptor_index{ m_handle.TxDescList.CurTxDesc };
        m_handle.ErrorCode = HAL_ETH_ERROR_NONE;
        // Enqueue without HAL's blocking timeout path, which clears OWN while
        // the DMA can still be reading the frame. HAL_ETH_Start leaves DMA
        // interrupts masked; completion is polled and released here instead.
        if (HAL_ETH_Transmit_IT(&m_handle, &packet) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        // Frames fit one buffer and these attributes need no context descriptor.
        m_pendingTransmitDescriptor = &m_handle.Init.TxDesc[descriptor_index];
        return waitForTransmit(started_at, timeout_ticks);
    }

    auto Ethernet::receive(std::span<std::byte> frame, std::chrono::milliseconds timeout) noexcept
      -> util::Result<std::size_t>
    {
        if (frame.empty() || timeout.count() < 0) {
            return error_result<std::size_t>(std::errc::invalid_argument);
        }

        ThreadXLock receive_lock{ m_receiveMutex };
        if (!isRunning()) {
            return error_result<std::size_t>(std::errc::not_connected);
        }

        const std::uint32_t timeout_ticks{ timeout_milliseconds(timeout) };
        const std::uint32_t started_at{ HAL_GetTick() };
        while (true) {
            void* packet_pointer{};
            if (HAL_ETH_ReadData(&m_handle, &packet_pointer) == HAL_OK && packet_pointer != nullptr) {
                auto* packet{ static_cast<ETH_BufferTypeDef*>(packet_pointer) };
                std::uint32_t receive_errors{};
                static_cast<void>(HAL_ETH_GetRxDataErrorCode(&m_handle, &receive_errors));
                // Each supported frame fits a DMA buffer. Filter before checking
                // the caller's capacity: unrelated traffic must not produce
                // no_buffer_space when the caller expects smaller frames.
                static_assert(MAX_FRAME_SIZE <= ETH_DMA_FRAME_BUFFER_SIZE);
                const std::span<const std::byte> first_buffer{
                    reinterpret_cast<const std::byte*>(packet->buffer), packet->len
                };
                if (receive_errors != 0U ||
                    (m_receiveEtherType && frameEtherType(first_buffer) != m_receiveEtherType)) {
                    release_frame(packet);
                    // Honor the deadline even in a stream of discarded frames.
                    if (timeout_ticks == 0U || HAL_GetTick() - started_at >= timeout_ticks) {
                        return error_result<std::size_t>(std::errc::timed_out);
                    }
                    continue;
                }
                std::size_t packet_size{};
                for (auto* current{ packet }; current != nullptr; current = current->next) {
                    packet_size += current->len;
                }

                if (packet_size > MAX_FRAME_SIZE || frame.size() < packet_size) {
                    release_frame(packet);
                    return error_result<std::size_t>(std::errc::no_buffer_space);
                }

                auto output{ frame.begin() };
                for (auto* current{ packet }; current != nullptr; current = current->next) {
                    output =
                      std::copy_n(reinterpret_cast<const std::byte*>(current->buffer), current->len, output);
                }
                release_frame(packet);

                return packet_size;
            }

            if (timeout_ticks == 0U || HAL_GetTick() - started_at >= timeout_ticks) {
                return error_result<std::size_t>(std::errc::timed_out);
            }
            tx_thread_sleep(1U);
        }
    }
}
