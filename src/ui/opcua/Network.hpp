#pragma once
#include <array>
#include "util/WifiProfile.hpp"
#include "ui/app/services/Types.hpp"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string_view>

namespace ui::opcua
{
    class Network final
    {
      public:
        enum class State : unsigned { Starting, Unavailable, Idle, Connecting, Connected, Failed };
        static constexpr bool wifiSupported()
        {
#if defined(CORE_CM7)
            return true;
#else
            return false;
#endif
        }
        struct Status
        {
            State state{ State::Starting };
            std::uint32_t address{};
            int result{};
            bool saved{}, busy{};
            std::array<char, 33> ssid{};
        };
        bool configure(std::string_view ssid, std::string_view password, bool persist);
        bool connect();
        bool disconnect();
        bool forget();
        struct AccessPoint { std::array<char,33> ssid{}; int signal{}; unsigned channel{}; };
        struct Scan {std::array<AccessPoint,16> entries{}; unsigned count{}, omitted{}; bool running{}; int result{};};
        bool scan();
        Scan scanStatus() const;
        Status status() const;
        services::NetworkDiagnostics diagnostics() const;
        void failed();
        void run(const std::stop_token& stop);
      private:
        enum class Request { None, Connect, Disconnect, Forget, Scan };
        using Credentials = util::WifiCredentials;
        std::uint64_t m_profileGeneration{};
        bool request(Request value);
        bool load();
        bool save(const Credentials& value);
        mutable std::mutex m_mutex;
        std::condition_variable_any m_changed;
        Credentials m_credentials{};
        Status m_status{};
        Scan m_scan{};
        Request m_request{};
        bool m_persist{};
        bool m_ready{};
    };
}
