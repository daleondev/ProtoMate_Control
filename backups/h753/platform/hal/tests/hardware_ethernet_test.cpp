#include "hal/drivers/factory/ethernet.hpp"
#include "hal/hal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>

extern "C" {
std::uint32_t hardware_ethernet_test_status{};
std::uint32_t hardware_ethernet_test_phase{};
[[gnu::noinline, gnu::used]] void hardware_ethernet_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;
    using Ethernet = hal::IEthernet;
    constexpr auto TEST_TYPE{ static_cast<Ethernet::EtherType>(0x88B5U) };
    using Frame = std::array<std::byte, Ethernet::MAX_FRAME_SIZE>;
    std::uint32_t sequence{};
    Ethernet::MacAddress peer{ 255U, 255U, 255U, 255U, 255U, 255U };

    template<typename... Args>
    void log(const char* format, Args... args)
    {
        std::printf("[ethernet-test] ");
        std::printf(format, args...);
        std::printf("\n");
        std::fflush(stdout);
    }

    void put16(Frame& frame, std::size_t offset, unsigned value)
    {
        frame[offset] = static_cast<std::byte>(value >> 8U);
        frame[offset + 1U] = static_cast<std::byte>(value);
    }

    Frame make_frame(Ethernet& ethernet, std::size_t size, unsigned tags, unsigned operation)
    {
        Frame frame{};
        const auto mac{ ethernet.getMacAddress() };
        for (std::size_t index{}; index < 6U; ++index) {
            frame[index] = static_cast<std::byte>(peer[index]);
            frame[6U + index] = static_cast<std::byte>(mac[index]);
        }
        std::size_t type_offset{ 12U };
        for (unsigned tag{}; tag < tags; ++tag) {
            put16(frame, type_offset, tags == 2U && tag == 0U ? 0x88A8U : 0x8100U);
            put16(frame, type_offset + 2U, 101U + tag);
            type_offset += 4U;
        }
        put16(frame, type_offset, 0x88B5U);
        const auto offset{ type_offset + 2U };
        frame[offset] = std::byte{ 'S' };
        frame[offset + 1U] = std::byte{ 'C' };
        frame[offset + 2U] = std::byte{ 'E' };
        frame[offset + 3U] = std::byte{ 'T' };
        frame[offset + 4U] = static_cast<std::byte>(operation);
        ++sequence;
        for (unsigned index{}; index < 4U; ++index) {
            frame[offset + 5U + index] = static_cast<std::byte>(sequence >> (24U - index * 8U));
        }
        put16(frame, offset + 9U, static_cast<unsigned>(size));
        for (std::size_t index{ 11U }; offset + index < size; ++index) {
            frame[offset + index] = static_cast<std::byte>((sequence * 31U + index * 17U) & 255U);
        }
        return frame;
    }

    bool verify_reply(Frame expected,
                      const Frame& received,
                      std::size_t size,
                      unsigned tags,
                      std::size_t received_size)
    {
        const auto wire_size{ std::max(size, Ethernet::MIN_FRAME_SIZE) };
        for (std::size_t index{}; index < 6U; ++index) {
            expected[index] = expected[6U + index];
            expected[6U + index] = received[6U + index];
        }
        expected[14U + tags * 4U + 4U] |= std::byte{ 128U };
        if (received_size != wire_size ||
            !std::equal(expected.begin(), expected.begin() + wire_size, received.begin())) {
            const auto mismatch{
                std::mismatch(expected.begin(), expected.begin() + wire_size, received.begin()).first -
                expected.begin()
            };
            log("payload mismatch seq=%lu sent=%u received=%u offset=%u",
                sequence,
                static_cast<unsigned>(size),
                static_cast<unsigned>(received_size),
                static_cast<unsigned>(mismatch));
            return false;
        }
        return true;
    }

    bool exchange(Ethernet& ethernet, std::size_t size, unsigned tags = 0U, unsigned operation = 2U)
    {
        auto frame{ make_frame(ethernet, size, tags, operation) };
        const auto sent{ ethernet.transmit(std::span{ frame }.first(size), 500ms) };
        if (!sent) {
            log("TX failed seq=%lu size=%u error=%d DMA=%08lx",
                sequence,
                static_cast<unsigned>(size),
                sent.error().value(),
                heth.Instance->DMACSR);
            return false;
        }
        Frame received{};
        const auto result{ ethernet.receive(received, 1000ms) };
        if (!result) {
            log("RX failed seq=%lu size=%u tags=%u error=%d DMA=%08lx last=%08lx",
                sequence,
                static_cast<unsigned>(size),
                tags,
                result.error().value(),
                heth.Instance->DMACSR,
                heth.RxDescList.pRxLastRxDesc);
            return false;
        }
        const auto reply_size{ operation == 7U ? 64U : size };
        if (operation == 7U) {
            put16(frame, 14U + tags * 4U + 9U, 64U);
        }
        if (!verify_reply(frame, received, reply_size, tags, *result)) {
            return false;
        }
        for (std::size_t index{}; index < 6U; ++index) {
            peer[index] = std::to_integer<std::uint8_t>(received[6U + index]);
        }
        return true;
    }

    bool wait_link(Ethernet& ethernet, unsigned speed = 0U)
    {
        const auto deadline{ std::chrono::steady_clock::now() + 15s };
        while (std::chrono::steady_clock::now() < deadline) {
            const auto link{ ethernet.getLinkInfo() };
            if (link && link->up && link->duplex != Ethernet::Duplex::Unknown &&
                (speed == 0U || link->speed_mbps == speed)) {
                log("link %lu Mb/s %s duplex",
                    link->speed_mbps,
                    link->duplex == Ethernet::Duplex::Full ? "full" : "half");
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        log("link timeout (requested %u Mb/s)", speed);
        return false;
    }

    void drain(Ethernet& ethernet)
    {
        Frame frame{};
        for (unsigned count{}; count < 32U && ethernet.receive(frame, 2ms); ++count) {
        }
    }

    bool discover(Ethernet& ethernet)
    {
        // The peer adapter can take longer than the PHY to become ready after
        // negotiation. Permit loss during discovery, then require exact replies.
        for (unsigned attempt{}; attempt < 10U; ++attempt) {
            if (exchange(ethernet, 60U, 0U, 1U)) {
                return true;
            }
        }
        log("peer discovery failed; MAC good TX=%lu RX CRC errors=%lu",
            heth.Instance->MMCTPCGR,
            heth.Instance->MMCRCRCEPR);
        return false;
    }

    bool maximum_frame_loopback(Ethernet& ethernet)
    {
        if (!ethernet.stop()) {
            return false;
        }
        ETH_MACConfigTypeDef original{};
        if (HAL_ETH_GetMACConfig(&heth, &original) != HAL_OK) {
            return false;
        }
        auto configuration{ original };
        configuration.LoopbackMode = ENABLE;
        bool passed{ HAL_ETH_SetMACConfig(&heth, &configuration) == HAL_OK && ethernet.start().has_value() };
        auto frame{ make_frame(ethernet, Ethernet::MAX_FRAME_SIZE, 2U, 2U) };
        std::copy_n(frame.begin() + 6U, 6U, frame.begin());
        Frame received{};
        passed = passed && ethernet.transmit(frame, 500ms).has_value();
        const auto result{ ethernet.receive(received, 1000ms) };
        passed = passed && result && *result == frame.size() && frame == received;
        if (!passed) {
            log("maximum RX failed: size=%u error=%d last=%08lx",
                result ? static_cast<unsigned>(*result) : 0U,
                result ? 0 : result.error().value(),
                heth.RxDescList.pRxLastRxDesc);
        }
        const bool stopped{ ethernet.stop().has_value() };
        const bool restored{ HAL_ETH_SetMACConfig(&heth, &original) == HAL_OK };
        return passed && stopped && restored;
    }

    bool run(Ethernet& ethernet)
    {
        hardware_ethernet_test_phase = 1U;
        if (!wait_link(ethernet) || !ethernet.start()) {
            return false;
        }
        const auto mac{ ethernet.getMacAddress() };
        log("MAC %02x:%02x:%02x:%02x:%02x:%02x; waiting for peer",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
        if (!discover(ethernet)) {
            return false;
        }
        hardware_ethernet_test_phase = 2U;
        for (const auto size : { 32U, 60U, 61U, 64U, 127U, 256U, 511U, 512U, 1024U, 1514U }) {
            if (!exchange(ethernet, size)) {
                return false;
            }
        }
        log("PASS untagged frame sizes and padding");
        hardware_ethernet_test_phase = 3U;
        for (const auto tags : { 1U, 2U }) {
            for (const auto size : { 64U, 512U, tags == 1U ? 1518U : 1514U }) {
                if (!exchange(ethernet, size, tags)) {
                    return false;
                }
            }
        }
        if (!exchange(ethernet, Ethernet::MAX_FRAME_SIZE, 2U, 7U)) {
            return false;
        }
        log("PASS single and double VLAN tags");
        hardware_ethernet_test_phase = 4U;
        if (!exchange(ethernet, 128U, 0U, 3U)) {
            return false;
        }
        const auto small_buffer_frame{ make_frame(ethernet, 512U, 0U, 4U) };
        if (!ethernet.transmit(std::span{ small_buffer_frame }.first(512U), 500ms)) {
            return false;
        }
        Frame received{};
        const auto undersized{ ethernet.receive(std::span{ received }.first(64U), 1000ms) };
        if (undersized || undersized.error() != std::errc::no_buffer_space) {
            log("small buffer did not report no_buffer_space");
            return false;
        }
        const auto next{ ethernet.receive(received, 1000ms) };
        if (!next || !verify_reply(small_buffer_frame, received, 512U, 0U, *next)) {
            return false;
        }
        const auto timeout{ ethernet.receive(received, 20ms) };
        if (timeout || timeout.error() != std::errc::timed_out) {
            return false;
        }
        // The peer sends an unrelated 512-byte frame before the matching one.
        // Only the matching frame may consume the caller's small buffer attempt.
        const auto filtered{ make_frame(ethernet, 512U, 0U, 3U) };
        if (!ethernet.transmit(std::span{ filtered }.first(512U), 500ms)) {
            return false;
        }
        const auto too_large{ ethernet.receive(std::span{ received }.first(64U), 1000ms) };
        if (too_large || too_large.error() != std::errc::no_buffer_space) {
            return false;
        }
        const auto remaining{ ethernet.receive(received, 20ms) };
        if (remaining || remaining.error() != std::errc::timed_out) {
            log("unrelated traffic consumed the small RX buffer attempt");
            return false;
        }
        log("PASS EtherType filtering, small RX buffer recovery, receive timeout");
        hardware_ethernet_test_phase = 5U;
        const auto began{ std::chrono::steady_clock::now() };
        for (unsigned iteration{}; iteration < 1000U; ++iteration) {
            const auto tags{ iteration % 3U };
            if (!exchange(ethernet, iteration % 2U ? (tags == 1U ? 1518U : 1514U) : 64U, tags)) {
                return false;
            }
        }
        log("PASS 1000 round trips in %lld ms",
            static_cast<long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began)
                .count()));
        hardware_ethernet_test_phase = 6U;
        for (unsigned iteration{}; iteration < 16U; ++iteration) {
            const auto frame{ make_frame(ethernet, 64U, 0U, 5U) };
            if (!ethernet.transmit(std::span{ frame }.first(64U), 500ms)) {
                return false;
            }
            std::this_thread::sleep_for(20ms);
            if (!ethernet.stop() || !ethernet.start()) {
                return false;
            }
            drain(ethernet);
            if (!exchange(ethernet, 256U)) {
                return false;
            }
        }
        log("PASS 16 restarts with queued RX frames");
        hardware_ethernet_test_phase = 7U;
        std::uint32_t advertisement{}, control{};
        if (HAL_ETH_ReadPHYRegister(&heth, 0U, 4U, &advertisement) != HAL_OK ||
            HAL_ETH_ReadPHYRegister(&heth, 0U, 0U, &control) != HAL_OK || !ethernet.stop()) {
            return false;
        }
        // LAN8742 ANAR bits 8:5 advertise 100/10 full/half duplex. Advertise
        // only 10BASE-T full duplex, then restore the original capabilities.
        bool passed{ HAL_ETH_WritePHYRegister(&heth, 0U, 4U, (advertisement & ~0x1E0U) | 0x40U) == HAL_OK &&
                     HAL_ETH_WritePHYRegister(&heth, 0U, 0U, control | 0x1200U) == HAL_OK };
        passed = passed && wait_link(ethernet, 10U) && ethernet.start().has_value() && discover(ethernet);
        for (unsigned iteration{}; passed && iteration < 32U; ++iteration) {
            passed = exchange(ethernet, 1514U);
        }
        const bool stopped{ ethernet.stop().has_value() };
        const bool restored{ HAL_ETH_WritePHYRegister(&heth, 0U, 4U, advertisement) == HAL_OK &&
                             HAL_ETH_WritePHYRegister(&heth, 0U, 0U, control | 0x1200U) == HAL_OK };
        if (!passed || !stopped || !restored || !wait_link(ethernet, 100U) || !ethernet.start() ||
            !discover(ethernet) || !exchange(ethernet, 1514U, 0U, 6U)) {
            return false;
        }
        log("PASS 10/100 Mb/s renegotiation and MAC mode changes");
        hardware_ethernet_test_phase = 8U;
        if (!maximum_frame_loopback(ethernet)) {
            return false;
        }
        log("PASS maximum double-tagged frame RX via internal loopback");
        return true;
    }
}

int main()
{
    const auto ethernet{ hal::ethernet::create({ .receive_ether_type = TEST_TYPE }) };
    const bool passed{ ethernet && run(*ethernet) };
    hardware_ethernet_test_status = passed ? 0x600D600DU : 0xBAD00000U | hardware_ethernet_test_phase;
    log("%s phase=%lu packets=%lu", passed ? "PASS" : "FAIL", hardware_ethernet_test_phase, sequence);
    hardware_ethernet_test_complete();
    return passed ? 0 : 1;
}
