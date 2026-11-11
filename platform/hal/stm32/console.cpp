#include "hal/console.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/stm32/UsbConsole.h"
#include <algorithm>
#include <array>
#include <cstdint>
#if defined(CORE_CM7)
#include "usbd_cdc_if.h"
#include <tx_api.h>
#include <tx_thread.h>
namespace
{
    constexpr unsigned INPUT_BYTES{ 4096 }, OUTPUT_BYTES{ 16384 }, PACKET_BYTES{ 64 },
      WORKER_STACK_BYTES{ 4096 }, STACK_ALIGNMENT{ 8 };
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(32) __attribute__((section(".ui_buffers.console"))) std::array<std::uint8_t, INPUT_BYTES> input;
    // A complete `stack ui` table with the GUI/touch workers exceeds 8 KiB.
    // Keep one bounded burst without waiting for a host or truncating its prompt.
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(32) __attribute__((section(".ui_buffers.console"))) std::array<std::uint8_t, OUTPUT_BYTES> output;
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    std::array<std::uint8_t, PACKET_BYTES> packet{};
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    std::uint32_t input_head{}, input_tail{}, output_head{}, output_tail{};
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    bool connected{}, dtr{}, suspended{}, in_flight{}, reset_pending{}, quarantine{}, started{};
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    std::uint32_t packet_length{};
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    TX_EVENT_FLAGS_GROUP events{};
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    TX_THREAD worker{};
    // Fixed USB callback/worker storage; access is serialized by the interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(STACK_ALIGNMENT) std::array<std::uint8_t, WORKER_STACK_BYTES> worker_stack{};
    void notify() noexcept
    {
        if (!started) {
            return;
        }
        // Keep ThreadX from enabling IRQs inside the console's PRIMASK guard.
        _tx_thread_preempt_disable = _tx_thread_preempt_disable + 1U;
        static_cast<void>(tx_event_flags_set(&events, 1U, TX_OR));
        _tx_thread_preempt_disable = _tx_thread_preempt_disable - 1U;
        if (_tx_thread_preempt_disable == 0 && _tx_thread_current_ptr != _tx_thread_execute_ptr) {
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
            SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
        }
    }
    void kick() noexcept
    {
        if (!connected || in_flight || output_head == output_tail) {
            return;
        }
        const auto count{ std::min<std::uint32_t>(packet.size(), output_head - output_tail) };
        for (std::uint32_t i{ 0 }; i < count; ++i) {
            packet[i] = output[(output_tail + i) % output.size()];
        }
        if (CDC_Transmit_FS(packet.data(), static_cast<std::uint16_t>(count)) == USBD_OK) {
            output_tail += count;
            in_flight = true;
            packet_length = count;
        }
    }
}
// Monotonic counter also readable when the host is absent or unresponsive.
extern "C" {
std::uint32_t giga_console_dropped_bytes{};
}
extern "C" void giga_console_reset()
{
    const hal::stm32::InterruptGuard guard{};
    connected = false;
    dtr = false;
    suspended = false;
    input_tail = input_head;
    giga_console_dropped_bytes += output_head - output_tail;
    output_tail = output_head;
    reset_pending = true;
    quarantine = false;
    notify();
}
extern "C" void giga_console_connected(int value)
{
    const hal::stm32::InterruptGuard guard{};
    if (!value) {
        giga_console_reset();
        return;
    }
    dtr = true;
    connected = !suspended;
    reset_pending = true;
    notify();
}
extern "C" void giga_console_suspend()
{
    const hal::stm32::InterruptGuard guard{};
    const bool previous_dtr{ dtr };
    giga_console_reset();
    dtr = previous_dtr;
    suspended = true;
}
extern "C" void giga_console_resume()
{
    const hal::stm32::InterruptGuard guard{};
    suspended = false;
    connected = dtr;
    reset_pending = true;
    notify();
}
extern "C" void giga_console_receive(const std::uint8_t* bytes, std::uint32_t count)
{
    const hal::stm32::InterruptGuard guard{};
    if (!connected) {
        return;
    }
    if (quarantine) {
        for (std::uint32_t i{ 0 }; i < count; ++i) {
            if (bytes[i] == '\n' || bytes[i] == '\r') {
                quarantine = false;
            }
        }
        return;
    }
    if (count > input.size() - (input_head - input_tail)) {
        input_tail = input_head;
        reset_pending = true;
        quarantine = true;
        notify();
        return; // Discard the entire packet; never execute its truncated tail.
    }
    for (std::uint32_t i{ 0 }; i < count; ++i) {
        input[input_head++ % input.size()] = bytes[i];
    }
}
extern "C" void giga_console_deinitialized()
{
    const hal::stm32::InterruptGuard guard{};
    if (in_flight) {
        giga_console_dropped_bytes += packet_length;
    }
    in_flight = false;
    giga_console_reset();
}
extern "C" void giga_console_transmitted()
{
    const hal::stm32::InterruptGuard guard{};
    in_flight = false;
    notify();
}
#endif

// Required newlib/linker ABI symbol. NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" int __io_putchar(int value)
{
#if defined(CORE_CM7)
    const hal::stm32::InterruptGuard guard{};
    if (!connected || output_head - output_tail == output.size()) {
        ++giga_console_dropped_bytes;
        return value; // Logging cannot stall on the USB host or set stdio error state.
    }
    output[output_head++ % output.size()] = static_cast<std::uint8_t>(value);
    notify();
    return value;
#else
    static_cast<void>(value);
    return -1;
#endif
}
// Required newlib/linker ABI symbol. NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" int __io_getchar()
{
    // The CLI uses try_read; libc's character hook is intentionally nonblocking.
    const auto value{ hal::console::try_read() };
    return value.state == hal::console::InputState::Character ? static_cast<unsigned char>(value.character)
                                                              : -1;
}
hal::console::Input hal::console::try_read() noexcept
{
#if defined(CORE_CM7)
    const stm32::InterruptGuard guard{};
    if (reset_pending) {
        reset_pending = false;
        return { .state = InputState::Reset };
    }
    if (input_head == input_tail) {
        return {};
    }
    return { .state = InputState::Character,
             .character = static_cast<char>(input[input_tail++ % input.size()]) };
#else
    return { .state = InputState::Closed };
#endif
}

void hal::console::initialize()
{
#if defined(CORE_CM7)
    const stm32::InterruptGuard guard{};
    if (started) {
        return;
    }
    static auto event_name{ std::to_array("usb.console.events") };
    static auto thread_name{ std::to_array("usb.console") };
    if (tx_event_flags_create(&events, event_name.data()) != TX_SUCCESS) {
        Error_Handler();
    }
    started = true;
    const auto entry{ +[](ULONG) {
        for (;;) {
            ULONG flags{};
            if (tx_event_flags_get(&events, 1, TX_OR_CLEAR, &flags, TX_WAIT_FOREVER) != TX_SUCCESS) {
                Error_Handler();
            }
            const stm32::InterruptGuard local{};
            kick();
        }
    } };
    if (tx_thread_create(&worker,
                         thread_name.data(),
                         entry,
                         0,
                         worker_stack.data(),
                         worker_stack.size(),
                         20,
                         20,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START) != TX_SUCCESS) {
        Error_Handler();
    }
    notify();
#endif
}
