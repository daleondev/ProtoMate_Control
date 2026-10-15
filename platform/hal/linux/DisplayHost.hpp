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
        std::uint64_t timeNs{};
        bool cancelled{};
    };
    // All functions cross the native/ThreadX boundary through native-safe
    // bounded mailboxes only. run() must execute on the original OS thread.
    using Observer = void (*)(void*, const hal::display::State&, std::uint32_t windowId);
    int run(void (*kernel)(), Observer observer = nullptr, void* context = nullptr);
    void completed(int status) noexcept;
    bool cleanupComplete() noexcept;
    bool available() noexcept;
    bool acquire() noexcept;
    void release() noexcept;
    std::span<std::uint16_t> framebuffer(unsigned slot) noexcept;
    bool submit(unsigned slot) noexcept;
    hal::display::State snapshot() noexcept;
    Pointer pointer() noexcept;
    std::uint32_t inputDrops() noexcept;
    void brightness(unsigned percent) noexcept;
}
