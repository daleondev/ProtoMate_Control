#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace ui::opcua::util
{
    struct WifiCredentials
    {
        std::array<char, 33> ssid{};
        std::array<char, 64> password{};
    };
    struct WifiProfile
    {
        using Record = std::array<unsigned char, 113>;
        WifiCredentials credentials{};
        std::uint64_t generation{};
        [[nodiscard]] Record encode() const noexcept;
        [[nodiscard]] static bool decode(std::span<const unsigned char> record, WifiProfile& output) noexcept;
    };
    // Erase credential temporaries even with optimization enabled.
    void erase_credentials(void* memory, std::size_t size) noexcept;
}
