#include "hal/console.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/stm32/UsbConsole.h"
#include <array>
#include <algorithm>
#include <cstdint>
#if defined(CORE_CM7)
#include "usbd_cdc_if.h"
#include <tx_api.h>
#include <tx_thread.h>
namespace {
    std::array<std::uint8_t, 4096> input{};
    std::array<std::uint8_t, 8192> output{};
    std::array<std::uint8_t, 64> packet{};
    std::uint32_t inputHead{}, inputTail{}, outputHead{}, outputTail{};
    bool connected{}, dtr{}, suspended{}, inFlight{}, resetPending{}, quarantine{}, started{};
    std::uint32_t packetLength{};
    TX_EVENT_FLAGS_GROUP events{};
    TX_THREAD worker{};
    alignas(8) std::array<std::uint8_t,4096> workerStack{};
    void notify() noexcept
    {
        if (!started) { return; }
        // Keep ThreadX from enabling IRQs inside the console's PRIMASK guard.
        _tx_thread_preempt_disable = _tx_thread_preempt_disable + 1u;
        static_cast<void>(tx_event_flags_set(&events, 1u, TX_OR));
        _tx_thread_preempt_disable = _tx_thread_preempt_disable - 1u;
        if (_tx_thread_preempt_disable == 0 && _tx_thread_current_ptr != _tx_thread_execute_ptr) {
            SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
        }
    }
    void kick() noexcept
    {
        if (!connected || inFlight || outputHead == outputTail) { return; }
        const auto count = std::min<std::uint32_t>(packet.size(), outputHead - outputTail);
        for (std::uint32_t i = 0; i < count; ++i) { packet[i] = output[(outputTail + i) % output.size()]; }
        if (CDC_Transmit_FS(packet.data(), static_cast<std::uint16_t>(count)) == USBD_OK) {
            outputTail += count;
            inFlight = true;
            packetLength = count;
        }
    }
}
// Monotonic counter also readable when the host is absent or unresponsive.
extern "C" { std::uint32_t giga_console_dropped_bytes{}; }
extern "C" void giga_console_reset()
{
    const hal::stm32::InterruptGuard guard;
    connected = false;
    dtr = false;
    suspended = false;
    inputTail = inputHead;
    giga_console_dropped_bytes += outputHead - outputTail;
    outputTail = outputHead;
    resetPending = true;
    quarantine = false;
    notify();
}
extern "C" void giga_console_connected(int value)
{
    const hal::stm32::InterruptGuard guard;
    if (!value) { giga_console_reset(); return; }
    dtr = true;
    connected = !suspended;
    resetPending = true;
    notify();
}
extern "C" void giga_console_suspend()
{
    const hal::stm32::InterruptGuard guard;
    const bool previousDtr = dtr;
    giga_console_reset();
    dtr = previousDtr;
    suspended = true;
}
extern "C" void giga_console_resume()
{
    const hal::stm32::InterruptGuard guard;
    suspended = false;
    connected = dtr;
    resetPending = true;
    notify();
}
extern "C" void giga_console_receive(const std::uint8_t* bytes, std::uint32_t count)
{
    const hal::stm32::InterruptGuard guard;
    if (!connected) { return; }
    if (quarantine) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (bytes[i] == '\n' || bytes[i] == '\r') { quarantine = false; }
        }
        return;
    }
    if (count > input.size() - (inputHead - inputTail)) {
        inputTail = inputHead;
        resetPending = true;
        quarantine = true;
        notify();
        return; // Discard the entire packet; never execute its truncated tail.
    }
    for (std::uint32_t i = 0; i < count; ++i) { input[inputHead++ % input.size()] = bytes[i]; }
}
extern "C" void giga_console_deinitialized()
{
    const hal::stm32::InterruptGuard guard;
    if (inFlight) { giga_console_dropped_bytes += packetLength; }
    inFlight = false;
    giga_console_reset();
}
extern "C" void giga_console_transmitted()
{
    const hal::stm32::InterruptGuard guard;
    inFlight = false;
    notify();
}
#endif

extern "C" int __io_putchar(int value)
{
#if defined(CORE_CM7)
    const hal::stm32::InterruptGuard guard;
    if (!connected || outputHead - outputTail == output.size()) {
        ++giga_console_dropped_bytes;
        return value; // Logging cannot stall on the USB host or set stdio error state.
    }
    output[outputHead++ % output.size()] = static_cast<std::uint8_t>(value);
    notify();
    return value;
#else
    static_cast<void>(value);
    return -1;
#endif
}
extern "C" int __io_getchar()
{
    // The CLI uses try_read; libc's character hook is intentionally nonblocking.
    const auto value = hal::console::try_read();
    return value.state == hal::console::InputState::Character ? static_cast<unsigned char>(value.character) : -1;
}
hal::console::Input hal::console::try_read() noexcept
{
#if defined(CORE_CM7)
    const stm32::InterruptGuard guard;
    if (resetPending) { resetPending = false; return {InputState::Reset}; }
    if (inputHead == inputTail) { return {}; }
    return {InputState::Character, static_cast<char>(input[inputTail++ % input.size()])};
#else
    return {InputState::Closed};
#endif
}

void hal::console::initialize()
{
#if defined(CORE_CM7)
    const stm32::InterruptGuard guard;
    if (started) { return; }
    static char eventName[] = "usb.console.events", threadName[] = "usb.console";
    if (tx_event_flags_create(&events, eventName) != TX_SUCCESS) { Error_Handler(); }
    started = true;
    const auto entry = +[](ULONG) {
        for (;;) {
            ULONG flags{};
            if (tx_event_flags_get(&events, 1, TX_OR_CLEAR, &flags, TX_WAIT_FOREVER) != TX_SUCCESS) { Error_Handler(); }
            const stm32::InterruptGuard local;
            kick();
        }
    };
    if (tx_thread_create(&worker, threadName, entry, 0, workerStack.data(), workerStack.size(),
        20, 20, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) { Error_Handler(); }
    notify();
#endif
}
