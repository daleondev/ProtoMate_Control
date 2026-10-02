#pragma once

#include "hal/drivers/factory/ethernet.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/hal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <thread>

namespace hardware_driver_checks
{
    using namespace std::chrono_literals;

    [[nodiscard]] inline auto gpio_output_toggle() -> bool
    {
        // PB0 is the onboard green LED. Its output latch must toggle even
        // when the released open-drain pin reads low through the pull-down.
        const auto output{ hal::gpio::createOutput({
          .pin = { .port = hal::gpio::Port::B, .number = 0U },
          .initial_level = hal::gpio::Level::High,
          .type = hal::gpio::OutputType::OpenDrain,
          .pull = hal::gpio::Pull::Down,
        }) };
        if (output == nullptr) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
        bool passed{ (GPIOB->ODR & GPIO_PIN_0) != 0U && output->read() == hal::gpio::Level::Low };
        output->toggle();
        passed = passed && (GPIOB->ODR & GPIO_PIN_0) == 0U;
        output->toggle();
        return passed && (GPIOB->ODR & GPIO_PIN_0) != 0U;
    }

    [[nodiscard]] inline auto gpio_interrupt_ownership() -> bool
    {
        constexpr hal::gpio::InputConfiguration configuration{
            .pin = { .port = hal::gpio::Port::C, .number = 13U },
            .pull = hal::gpio::Pull::Down,
            .edge = hal::gpio::Edge::Rising,
        };
        auto input{ hal::gpio::createInput(configuration) };
        if (input == nullptr) {
            return false;
        }

        HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
        __HAL_GPIO_EXTI_GENERATE_SWIT(GPIO_PIN_13);
        input.reset();
        const bool cleared{ __HAL_GPIO_EXTI_GET_IT(GPIO_PIN_13) == 0U };

        input = hal::gpio::createInput(configuration);
        if (input == nullptr) {
            return false;
        }
        std::atomic_uint callbacks{};
        input->setEdgeCallback(
          [&callbacks](hal::gpio::Level) noexcept { callbacks.fetch_add(1U, std::memory_order_relaxed); });
        std::this_thread::sleep_for(1ms);
        bool passed{ cleared && callbacks.load(std::memory_order_relaxed) == 0U };
        __HAL_GPIO_EXTI_GENERATE_SWIT(GPIO_PIN_13);
        std::this_thread::sleep_for(1ms);
        passed = passed && callbacks.load(std::memory_order_relaxed) == 1U;
        input->clearEdgeCallback();
        return passed;
    }

    [[nodiscard]] inline auto ethernet_loopback() -> bool
    {
        const auto ethernet{ hal::ethernet::create() };
        if (ethernet == nullptr || ethernet->isRunning()) {
            return false;
        }
        if (hal::ethernet::create({ .phy_address = 32U }) != nullptr ||
            hal::ethernet::create({ .receive_ether_type = hal::IEthernet::EtherType::IPv4 }) != nullptr) {
            return false;
        }

        ETH_MACConfigTypeDef original{};
        if (HAL_ETH_GetMACConfig(&heth, &original) != HAL_OK) {
            return false;
        }
        auto loopback{ original };
        loopback.LoopbackMode = ENABLE;
        if (HAL_ETH_SetMACConfig(&heth, &loopback) != HAL_OK) {
            return false;
        }

        bool passed{ ethernet->start().has_value() };
        std::array<std::byte, hal::IEthernet::MIN_FRAME_SIZE> frame{};
        std::array<std::byte, hal::IEthernet::MAX_FRAME_SIZE> received{};
        const auto address{ ethernet->getMacAddress() };
        for (std::size_t index{}; index < address.size(); ++index) {
            frame[index] = static_cast<std::byte>(address[index]);
            frame[address.size() + index] = static_cast<std::byte>(address[index]);
        }
        frame[12] = std::byte{ 0x88 };
        frame[13] = std::byte{ 0xA4 };
        // Wrap both DMA descriptor rings repeatedly and exercise restart.
        for (unsigned int packet{}; passed && packet < 12U; ++packet) {
            frame[14] = static_cast<std::byte>(packet);
            passed = ethernet->transmit(frame, 100ms).has_value();
            const auto result{ ethernet->receive(received, 100ms) };
            passed = passed && result && *result == frame.size() &&
                     std::equal(frame.begin(), frame.end(), received.begin());
            if (packet == 5U) {
                const bool stopped{ ethernet->stop().has_value() };
                const bool restarted{ ethernet->start().has_value() };
                passed = passed && stopped && restarted;
            }
        }
        if (passed) {
            // Exercise the advertised maximum frame including two VLAN tags.
            std::array<std::byte, hal::IEthernet::MAX_FRAME_SIZE> tagged{};
            std::copy(frame.begin(), frame.end(), tagged.begin());
            tagged[12] = std::byte{ 0x88 };
            tagged[13] = std::byte{ 0xA8 };
            tagged[14] = tagged[15] = std::byte{};
            tagged[16] = std::byte{ 0x81 };
            tagged[17] = tagged[18] = tagged[19] = std::byte{};
            tagged[20] = std::byte{ 0x88 };
            tagged[21] = std::byte{ 0xA4 };
            passed = ethernet->transmit(tagged, 100ms).has_value();
            const auto result{ ethernet->receive(received, 100ms) };
            passed = passed && result && *result == tagged.size() && tagged == received;

            // An unrelated large frame must be filtered before applying the
            // caller's receive-buffer limit.
            tagged[21] = std::byte{ 0xB5 };
            passed = passed && ethernet->transmit(tagged, 100ms).has_value();
            const auto filtered{ ethernet->receive(frame, 10ms) };
            passed = passed && !filtered && filtered.error() == std::errc::timed_out;
        }
        for (unsigned queued{ 1U }; passed && queued <= ETH_RX_DESC_CNT; ++queued) {
            // Leave the software and DMA cursors at different ring positions.
            // stop/start must discard queued frames and accept the next packet.
            for (unsigned packet{}; passed && packet < queued; ++packet) {
                passed = ethernet->transmit(frame, 100ms).has_value();
            }
            std::this_thread::sleep_for(2ms);
            passed = passed && ethernet->stop().has_value() && ethernet->start().has_value();
            frame[14] = static_cast<std::byte>(queued);
            passed = passed && ethernet->transmit(frame, 100ms).has_value();
            const auto result{ ethernet->receive(received, 100ms) };
            passed = passed && result && *result == frame.size() &&
                     std::equal(frame.begin(), frame.end(), received.begin());
        }
        if (passed) {
            // Hold DMA idle so a zero-timeout send deterministically leaves a
            // packet pending. A retry must preserve its buffer until DMA has
            // consumed it, then send the retry's contents as a second frame.
            CLEAR_BIT(heth.Instance->DMACTCR, ETH_DMACTCR_ST);
            __DSB();
            frame[14] = std::byte{ 0xA5 };
            const auto first{ ethernet->transmit(frame, 0ms) };
            frame[14] = std::byte{ 0x5A };
            const auto retry{ ethernet->transmit(frame, 0ms) };
            passed = !first && first.error() == std::make_error_code(std::errc::timed_out) && !retry &&
                     retry.error() == std::make_error_code(std::errc::timed_out);
            SET_BIT(heth.Instance->DMACTCR, ETH_DMACTCR_ST);
            WRITE_REG(heth.Instance->DMACTDTPR, heth.TxDescList.TxDesc[heth.TxDescList.CurTxDesc]);

            const auto completed{ ethernet->transmit(frame, 100ms) };
            const auto first_received{ ethernet->receive(received, 100ms) };
            passed = passed && completed && first_received && *first_received == frame.size() &&
                     received[14] == std::byte{ 0xA5 };
            const auto retry_received{ ethernet->receive(received, 100ms) };
            passed = passed && retry_received && *retry_received == frame.size() &&
                     std::equal(frame.begin(), frame.end(), received.begin()) &&
                     heth.TxDescList.BuffersInUse == 0U;
        }
        if (passed) {
            CLEAR_BIT(heth.Instance->DMACTCR, ETH_DMACTCR_ST);
            __DSB();
            const auto pending{ ethernet->transmit(frame, 0ms) };
            passed = !pending && pending.error() == std::errc::timed_out && ethernet->stop().has_value() &&
                     ethernet->start().has_value() && ethernet->transmit(frame, 100ms).has_value();
            const auto result{ ethernet->receive(received, 100ms) };
            passed = passed && result && *result == frame.size() &&
                     std::equal(frame.begin(), frame.end(), received.begin()) &&
                     heth.TxDescList.BuffersInUse == 0U;
        }
        const bool stopped{ ethernet->stop().has_value() };
        const bool restored{ HAL_ETH_SetMACConfig(&heth, &original) == HAL_OK };
        return passed && stopped && restored;
    }
}
