#pragma once

#include <cstdint>

namespace hal::console
{
    enum class InputState : std::uint8_t
    {
        Idle,
        Character,
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
    // STM32: USART3 on M7; M4 has no console and reports Closed.
    [[nodiscard]] Input try_read() noexcept;
}
