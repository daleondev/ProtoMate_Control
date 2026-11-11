#include "Network.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <limits>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#if defined(CORE_CM7)
#include "network/Wifi.h"
#include "network/Memory.h"
#endif

namespace ui::opcua
{
    namespace
    {
        constexpr const char* PROFILE{ "/flash/network.cfg" };
        constexpr std::array<const char*, 2> PROFILE_SLOTS{ "/flash/network0.cfg", "/flash/network1.cfg" };
        using util::erase_credentials;
        std::uint32_t checksum(const void* memory, std::size_t size)
        {
            std::uint32_t value{ 2166136261U };
            const auto* bytes{ static_cast<const unsigned char*>(memory) };
            while (size--) value = (value ^ *bytes++) * 16777619U;
            return value;
        }
        // Explicit byte layout, including checksum, rather than native padding.
        using Record = std::array<unsigned char, 105>;
        constexpr std::size_t SSID_OFFSET{ 4 }, PASSWORD_OFFSET{ 37 }, CRC_OFFSET{ 101 };
        bool read_profile(const char* path, util::WifiProfile& result)
        {
            auto* file{ std::fopen(path, "rb") };
            if (!file) return false;
            util::WifiProfile::Record record{};
            const bool read{ std::fread(record.data(), record.size(), 1, file) == 1 && std::fgetc(file) == EOF };
            const bool valid{ read && util::WifiProfile::decode(record, result) };
            std::fclose(file);
            erase_credentials(record.data(), record.size());
            return valid;
        }
    }
    Network::Status Network::status() const { std::scoped_lock lock{ m_mutex }; return m_status; }
    services::NetworkDiagnostics Network::diagnostics() const
    {
        const auto value{status()};
        services::NetworkDiagnostics out{.connected = value.state == State::Connected, .busy = value.busy,
            .saved = value.saved, .state = static_cast<std::uint32_t>(value.state), .address = value.address, .error = value.result};
#if defined(CORE_CM7)
        out.wifi = true;
        const auto memory{network_memory_stats()};
        out.capacity = memory.capacity; out.used = memory.used; out.peak = memory.peak; out.failures = memory.failures;
        if (value.state == State::Connected) {
            if (auto* pool{wifi_packet_pool()}) {
                ULONG total{}, available{}, empty{}, suspensions{}, invalid{};
                if (nx_packet_pool_info_get(pool, &total, &available, &empty, &suspensions, &invalid) == NX_SUCCESS) {
                    out.packets = total; out.available_packets = available; out.empty_packet_requests = empty;
                    out.packet_suspensions = suspensions; out.invalid_packet_releases = invalid;
                }
            }
            if (auto* pool{wifi_receive_pool()}) {
                ULONG total{}, available{}, empty{}, suspensions{}, invalid{};
                if (nx_packet_pool_info_get(pool, &total, &available, &empty, &suspensions, &invalid) == NX_SUCCESS) {
                    out.receive_packets = total; out.receive_available = available; out.receive_empty_requests = empty;
                }
            }
        }
#endif
        return out;
    }
    void Network::failed()
    {
        std::scoped_lock lock{m_mutex};
        m_ready = false; m_status.busy = false; m_status.address = 0;
        m_status.state = State::Failed; m_status.result = -1;
    }
    bool Network::configure(std::string_view ssid, std::string_view password, bool persist)
    {
        if (ssid.empty() || ssid.size() > 32 || password.size() < 8 || password.size() > 63 ||
            ssid.find('\0') != std::string_view::npos || password.find('\0') != std::string_view::npos) return false;
        std::scoped_lock lock{ m_mutex };
        if (!m_ready || m_status.busy || m_request != Request::None) return false;
        erase_credentials(&m_credentials, sizeof(m_credentials));
        std::memcpy(m_credentials.ssid.data(), ssid.data(), ssid.size());
        std::memcpy(m_credentials.password.data(), password.data(), password.size());
        m_persist = persist;
        m_request = Request::Connect;
        m_changed.notify_all();
        return true;
    }
    bool Network::request(Request value)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_ready || m_status.busy || m_request != Request::None) return false;
        m_request = value;
        m_changed.notify_all();
        return true;
    }
    bool Network::connect() { return request(Request::Connect); }
    bool Network::disconnect() { return request(Request::Disconnect); }
    bool Network::forget() { return request(Request::Forget); }
    bool Network::scan() {return request(Request::Scan);}
    Network::Scan Network::scanStatus() const {std::scoped_lock lock{m_mutex}; return m_scan;}
    bool Network::load()
    {
        util::WifiProfile latest{};
        for (auto path : PROFILE_SLOTS) {
            util::WifiProfile candidate{};
            if (read_profile(path, candidate) && candidate.generation > latest.generation) latest = candidate;
            erase_credentials(&candidate, sizeof(candidate));
        }
        if (latest.generation) {
            m_credentials = latest.credentials;
            m_profileGeneration = latest.generation;
            erase_credentials(&latest, sizeof(latest));
            return true;
        }
        // Read the initial stage-1 profile once; new writes use two committed
        // slots so an interrupted replacement retains the preceding profile.
        auto* file{ std::fopen(PROFILE, "rb") };
        if (!file) return false;
        Record record{};
        const bool read{ std::fread(record.data(), record.size(), 1, file) == 1 && std::fgetc(file) == EOF };
        std::uint32_t crc{};
        for (unsigned i{}; i < 4; ++i) crc |= static_cast<std::uint32_t>(record[CRC_OFFSET + i]) << (i * 8);
        const bool valid{ read && record[0] == 1 && record[1] == 0 && record[2] == 0 && record[3] == 0 &&
            record[PASSWORD_OFFSET - 1] == 0 && record[CRC_OFFSET - 1] == 0 &&
            crc == checksum(record.data(), CRC_OFFSET) && record[SSID_OFFSET] != 0 &&
            std::strlen(reinterpret_cast<const char*>(record.data() + PASSWORD_OFFSET)) >= 8 };
        std::fclose(file);
        if (valid) {
            std::memcpy(m_credentials.ssid.data(), record.data() + SSID_OFFSET, m_credentials.ssid.size());
            std::memcpy(m_credentials.password.data(), record.data() + PASSWORD_OFFSET, m_credentials.password.size());
        }
        erase_credentials(&record, sizeof(record));
        return valid;
    }
    bool Network::save(const Credentials& value)
    {
        if (m_profileGeneration == std::numeric_limits<std::uint64_t>::max()) return false;
        util::WifiProfile profile{ value, m_profileGeneration + 1 };
        auto record{ profile.encode() };
        // Only overwrite the older slot. The other valid generation remains
        // readable throughout truncation, writing, flushing and power loss.
        const auto descriptor{ ::open(PROFILE_SLOTS[profile.generation % PROFILE_SLOTS.size()], O_WRONLY | O_CREAT | O_TRUNC, 0600) };
        bool result{ descriptor >= 0 };
        if (result) {
            std::size_t offset{};
            while (offset < record.size()) {
                const auto written{ ::write(descriptor, record.data() + offset, record.size() - offset) };
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { result = false; break; }
                offset += static_cast<std::size_t>(written);
            }
            if (result) result = ::fsync(descriptor) == 0;
            result = ::close(descriptor) == 0 && result;
            if (result) {
                m_profileGeneration = profile.generation;
                std::remove(PROFILE);
            }
        }
        erase_credentials(&profile, sizeof(profile));
        erase_credentials(&record, sizeof(record));
        return result;
    }

    void Network::run(const std::stop_token& stop)
    {
#if defined(CORE_CM7)
        const int initialized{ wifi_initialize() };
        {
            std::scoped_lock lock{ m_mutex };
            m_status.state = initialized ? State::Failed : State::Idle;
            m_status.result = initialized;
            m_ready = initialized == 0;
            if (!initialized && m_request == Request::None && load()) {
                m_status.saved = true;
                m_request = Request::Connect;
            }
        }
        if (initialized) return;
        bool reconnect{ false };
        auto nextRetry{ std::chrono::steady_clock::now() };
        while (!stop.stop_requested()) {
            Credentials credentials{};
            Request operation{};
            bool persist{};
            {
                std::unique_lock lock{ m_mutex };
                m_changed.wait_for(lock, stop, std::chrono::milliseconds{ 100 }, [this] { return m_request != Request::None; });
                if (stop.stop_requested()) break;
                operation = m_request;
                m_request = Request::None;
                if (m_status.state == State::Connected && !wifi_connected()) {
                    m_status.state = State::Idle;
                    m_status.address = 0;
                }
                if (operation == Request::None && reconnect && !wifi_connected() &&
                    std::chrono::steady_clock::now() >= nextRetry) operation = Request::Connect;
                if (operation == Request::None) continue;
                m_status.busy = true;
                if (operation != Request::Scan) {
                    m_status.state = operation == Request::Connect ? State::Connecting : State::Idle;
                    m_status.address = 0;
                }
                credentials = m_credentials;
                if (operation == Request::Connect) m_status.ssid = credentials.ssid;
                persist = m_persist; m_persist = false;
            }
            int result{};
            bool saved{};
            if (operation == Request::Scan) {
                {
                    std::scoped_lock lock{m_mutex}; m_scan = {}; m_scan.running = true;
                }
                result = wifi_scan_start();
                WifiScan scan{};
                const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{10}};
                while (!result) {
                    wifi_scan_snapshot(&scan);
                    if (!scan.running) break;
                    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                        static_cast<void>(wifi_scan_stop()); result = -ETIMEDOUT; break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                }
                {
                    std::scoped_lock lock{m_mutex};
                    m_scan.count = std::min<unsigned>(scan.count, m_scan.entries.size());
                    m_scan.omitted = scan.omitted; m_scan.running = false; m_scan.result = result;
                    for (unsigned i{}; i < m_scan.count; ++i) {
                        std::copy_n(scan.entries[i].ssid, 33, m_scan.entries[i].ssid.begin());
                        m_scan.entries[i].signal = scan.entries[i].rssi; m_scan.entries[i].channel = scan.entries[i].channel;
                    }
                    m_status.busy = false;
                }
                erase_credentials(&credentials, sizeof(credentials));
                continue;
            }
            if (operation == Request::Connect) {
                if (wifi_connected()) wifi_disconnect();
                result = wifi_join(credentials.ssid.data(), credentials.password.data());
                // An AP absent during cold startup must not permanently
                // prevent the saved profile from reconnecting later.
                reconnect = credentials.ssid[0] != 0;
                if (!result && persist) saved = save(credentials);
                nextRetry = std::chrono::steady_clock::now() + std::chrono::seconds{ 5 };
            } else {
                reconnect = false;
                result = wifi_disconnect();
                if (operation == Request::Forget) {
                    auto remove = [&](const char* path) {
                        if (std::remove(path) != 0 && errno != ENOENT) result = -errno;
                    };
                    remove(PROFILE);
                    for (auto path : PROFILE_SLOTS) remove(path);
                    wifi_forget_credentials();
                    if (!result) m_profileGeneration = 0;
                }
            }
            {
                std::scoped_lock lock{ m_mutex };
                m_status.busy = false;
                m_status.result = result;
                m_status.state = operation == Request::Connect && !result ? State::Connected : (result ? State::Failed : State::Idle);
                if (m_status.state == State::Connected) {
                    ULONG address{}, mask{};
                    nx_ip_address_get(wifi_ip(), &address, &mask);
                    m_status.address = address;
                    m_status.ssid = credentials.ssid;
                }
                if (persist) m_status.saved = saved;
                if (operation == Request::Forget) {
                    erase_credentials(&m_credentials, sizeof(m_credentials));
                    m_status.ssid = {};
                    if (!result) m_status.saved = false;
                }
            }
            erase_credentials(&credentials, sizeof(credentials));
        }
        wifi_disconnect();
#else
        std::unique_lock lock{ m_mutex };
        m_status.state = State::Connected;
        m_status.address = 0x7F000001U;
        m_changed.wait(lock, stop, [] { return false; });
#endif
    }
}
