#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace ui::gui::hmi
{
    inline constexpr std::string_view NETWORK_KEYS{
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"};
    enum class NetworkAction { Scan, Connect, Disconnect, Forget, StartServer, StopServer, AllowRemote, RevokeRemote };
    struct NetworkStatus
    {
        bool wifi{}, connected{}, busy{}, saved{}, server{}, remote{}, scanning{};
        unsigned clients{}, scan_count{}, scan_omitted{};
        int error{}, scan_error{};
        std::uint32_t address{};
        std::array<char, 33> ssid{};
    };
    struct NetworkEntry { std::array<char, 33> ssid{}; int signal{}; unsigned channel{}; };
    // Only the active GUI editor holds a private copy. Nothing is sent to
    // message history, telemetry, snapshots, or the OPC UA address space.
    struct NetworkForm
    {
        std::array<char, 33> ssid{};
        std::array<char, 64> password{}, editor{};
        std::uint8_t editing{}, key_page{}, scan_page{};
        void clear() noexcept
        {
            for (auto& c : password) *static_cast<volatile char*>(&c) = 0;
            for (auto& c : editor) *static_cast<volatile char*>(&c) = 0;
            ssid.fill(0);
            editing = key_page = scan_page = 0;
        }
        void clearEditor() noexcept
        {
            for (auto& c : editor) *static_cast<volatile char*>(&c) = 0;
            editing = 0;
        }
    };
}
