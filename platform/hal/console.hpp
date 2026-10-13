#pragma once

#include <cstdint>

namespace hal::console
{
    enum class InputState : std::uint8_t
    {
        Idle,
        Character,
        Reset, // Discard an incomplete command; the console remains available.
        Closed,
        Error
    };
    struct Input
    {
        InputState state{ InputState::Idle };
        char character{};
    };

    // Nonblocking input from the platform's existing standard console. One
    // reader only; do not combine with simultaneous std::cin/_read calls.
    // STM32: USB CDC on M7; M4 has no console and reports Closed.
    // Start the hardware console worker from application thread context. Idempotent.
    void initialize();
    [[nodiscard]] Input try_read() noexcept;
}
