#pragma once
#include "hal/drivers/itf/DisplayEvents.hpp"
#include <cstdint>
#include <span>

namespace hal::linux::display
{
    struct Pointer
    {
        bool down{};
        std::uint16_t x{}, y{};
        std::uint64_t time_ns{};
        bool cancelled{};
        float wheel{};
    };
    // All functions cross the native/ThreadX boundary through native-safe
    // bounded mailboxes only. run() must execute on the original OS thread.
    using Observer = void (*)(void*, const hal::display::State&, std::uint32_t window_id);
    int run(void (*kernel)(), Observer observer = nullptr, void* context = nullptr);
    void completed(int status) noexcept;
    bool cleanup_complete() noexcept;
    bool available() noexcept;
    // Optional peripheral clock. It must be native-thread safe and remain valid
    // until release(); the SDL event pump always retains its wall-clock pacing.
    using Clock = std::uint64_t (*)() noexcept;
    bool acquire(Clock clock = nullptr) noexcept;
    void release() noexcept;
    std::span<std::uint16_t> framebuffer(unsigned slot) noexcept;
    bool submit(unsigned slot) noexcept;
    hal::display::State snapshot() noexcept;
    Pointer pointer() noexcept;
    // Numeric editor session (zero means closed). A new session discards old
    // keys. Focus loss/overflow cancels the editor rather than applying partial input.
    unsigned keyboard(std::uint32_t session, std::span<char> output) noexcept;
    std::uint32_t input_drops() noexcept;
    void brightness(unsigned percent) noexcept;
}
