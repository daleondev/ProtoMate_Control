#include "Wifi.h"
#include "Echo.h"
#include "Server.hpp"
#include "hal/console.hpp"
#include "runtime/thread.hpp"

#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <malloc.h>
#include <mutex>

extern "C" { extern std::byte __HeapBase, __HeapLimit; }

namespace
{
    enum class Request { None, Join, Disconnect, Reconnect, Scan, Start, Stop, Echo };
    std::atomic<Request> request{ Request::None };
    std::atomic<int> state{ 0 }; // 0 starting, 1 ready, 2 joining, 3 connected, -1 failed
    std::atomic<int> lastError{};
    std::array<char, 33> ssid{};
    std::array<char, 64> password{};
    alignas(std::max_align_t) __attribute__((section(".dtcm_bss.opcua_stack")))
        std::array<std::byte, 12 * 1024> workerStack;
    struct Snapshot {
        OpcUaMemoryStats ua{};
        ULONG address{}, packets{}, poolFree{}, poolLow{ ~0UL }, poolEmpty{}, invalidRelease{}, echoBytes{};
        std::size_t stackPeak{};
        bool server{};
    } snapshot;
    std::mutex snapshotMutex;

    void clearPassword()
    {
        volatile char* target{ password.data() };
        for (std::size_t i{}; i < password.size(); ++i) target[i] = 0;
    }

    void network(std::stop_token token)
    {
        const auto result{ wifi_initialize() };
        lastError.store(result);
        state.store(result == 0 ? 1 : -1);
        auto mode{ Request::Start };
        bool wasConnected{};
        auto publishAt{ std::chrono::steady_clock::now() };
        while (!token.stop_requested()) {
            const auto pending{ request.load() };
            if (pending == Request::Join) {
                state.store(2);
                lastError.store(wifi_join(ssid.data(), password.data()));
                clearPassword();
                state.store(wifi_connected() ? 3 : 1);
                request.store(Request::None);
            } else if (pending == Request::Disconnect) {
                static_cast<void>(sample::stopServer());
                sample_echo_stop();
                lastError.store(wifi_disconnect());
                state.store(1);
                request.store(Request::None);
            } else if (pending == Request::Reconnect) {
                static_cast<void>(sample::stopServer());
                sample_echo_stop();
                wasConnected = false;
                state.store(2);
                if (wifi_connected()) static_cast<void>(wifi_disconnect());
                lastError.store(wifi_reconnect());
                state.store(wifi_connected() ? 3 : 1);
                request.store(Request::None);
            } else if (pending == Request::Scan) {
                lastError.store(wifi_scan_start());
                request.store(Request::None);
            } else if (pending == Request::Start || pending == Request::Stop || pending == Request::Echo) {
                lastError.store(static_cast<int>(sample::stopServer()));
                sample_echo_stop();
                mode = pending;
                wasConnected = false;
                request.store(Request::None);
            }
            const bool connected{ wifi_connected() != 0 };
            if (connected && !wasConnected) {
                if (mode == Request::Start) lastError.store(static_cast<int>(sample::startServer()));
                else if (mode == Request::Echo) lastError.store(static_cast<int>(sample_echo_start()));
                state.store(3);
            } else if (!connected && wasConnected) {
                static_cast<void>(sample::stopServer());
                sample_echo_stop();
                state.store(1);
            }
            wasConnected = connected;
            if (connected && mode == Request::Echo) sample_echo_poll();
            if (sample::serverRunning()) sample::pollServer();
            else std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
            if (std::chrono::steady_clock::now() >= publishAt) {
                Snapshot next{};
                next.ua = opcua_memory_stats();
                next.server = sample::serverRunning();
                next.echoBytes = sample_echo_bytes();
                if (auto* ip{ wifi_ip() }; ip && connected) {
                    ULONG mask{};
                    nx_ip_address_get(ip, &next.address, &mask);
                    if (auto* pool{ wifi_packet_pool() })
                        nx_packet_pool_info_get(pool, &next.packets, &next.poolFree, &next.poolEmpty,
                                                nullptr, &next.invalidRelease);
                }
                auto* thread{ tx_thread_identify() };
                if (thread) {
                    const auto top{ reinterpret_cast<std::uintptr_t>(thread->tx_thread_stack_end) + 1U };
                    const auto peak{ reinterpret_cast<std::uintptr_t>(thread->tx_thread_stack_highest_ptr) };
                    if (peak && peak <= top) next.stackPeak = top - peak;
                }
                {
                    const std::lock_guard lock{ snapshotMutex };
                    next.poolLow = next.packets ? std::min(snapshot.poolLow, next.poolFree) : snapshot.poolLow;
                    next.stackPeak = std::max(snapshot.stackPeak, next.stackPeak);
                    snapshot = next;
                }
                publishAt = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 250 };
            }
        }
        static_cast<void>(sample::stopServer());
        sample_echo_stop();
    }

    void status()
    {
        const auto heap{ mallinfo() };
        const auto capacity{ reinterpret_cast<std::uintptr_t>(&__HeapLimit) -
                             reinterpret_cast<std::uintptr_t>(&__HeapBase) };
        std::printf("[opcua] network=%d result=0x%08x heap_used=%zu heap_available=%zu\r\n",
                    state.load(), static_cast<unsigned>(lastError.load()),
                    static_cast<std::size_t>(heap.uordblks), capacity - heap.arena + heap.fordblks);
        Snapshot value;
        { const std::lock_guard lock{ snapshotMutex }; value = snapshot; }
        const auto ip{ value.address };
        if (ip) std::printf("[opcua] address=%lu.%lu.%lu.%lu\r\n", ip >> 24, (ip >> 16) & 255,
                            (ip >> 8) & 255, ip & 255);
        std::printf("[opcua] server=%d ua_used=%zu ua_peak=%zu ua_capacity=%zu ua_failures=%zu\r\n",
            value.server, value.ua.used, value.ua.peak, value.ua.capacity, value.ua.failures);
        std::printf("[opcua] packets=%lu free=%lu low=%lu empty=%lu invalid_release=%lu echo_bytes=%lu stack_peak=%zu/%zu\r\n",
            value.packets, value.poolFree, value.poolLow, value.poolEmpty, value.invalidRelease,
            value.echoBytes, value.stackPeak, workerStack.size());
    }
}

int main()
{
    hal::console::initialize();
    auto worker{ runtime::thread::create_jthread(
        { .name = "WiFi OPC UA", .priority = 20, .stack_size = workerStack.size(),
          .stack_storage = workerStack.data() }, network) };
    std::array<char, 96> line{};
    std::size_t length{};
    enum class Input { Command, Ssid, Password };
    Input input{ Input::Command };
    bool overflow{};
    std::printf("\r\n[opcua] Standalone sample; motors disabled. Anonymous/unencrypted, trusted LAN only.\r\n"
                "wifi | status | start | stop | echo | disconnect | help\r\n> ");
    std::fflush(stdout);
    while (true) {
        const auto next{ hal::console::try_read() };
        if (next.state == hal::console::InputState::Reset || next.state == hal::console::InputState::Closed) {
            length = 0;
            overflow = false;
            input = Input::Command;
            line.fill(0);
            if (request.load() == Request::None) clearPassword();
        } else if (next.state == hal::console::InputState::Character) {
            const auto c{ next.character };
            if (c == '\r' || c == '\n') {
                if (!length && !overflow) continue;
                std::printf("\r\n");
                if (overflow) {
                    std::printf("Input too long; discarded.\r\n");
                    input = Input::Command;
                } else if (input == Input::Ssid) {
                    if (length > 32) {
                        std::printf("SSID must be 1-32 bytes.\r\n");
                        input = Input::Command;
                    } else {
                        std::snprintf(ssid.data(), ssid.size(), "%s", line.data());
                        input = Input::Password;
                        std::printf("Password (hidden): ");
                    }
                } else if (input == Input::Password) {
                    if (length < 8 || length > 63) std::printf("Password must be 8-63 bytes.\r\n");
                    else {
                        std::snprintf(password.data(), password.size(), "%s", line.data());
                        request.store(Request::Join);
                        std::printf("Association requested. Use status for the result.\r\n");
                    }
                    input = Input::Command;
                } else if (std::strcmp(line.data(), "wifi") == 0) {
                    if (state.load() != 1 || request.load() != Request::None) std::printf("Not ready; use status.\r\n");
                    else { input = Input::Ssid; std::printf("SSID: "); }
                } else if (std::strcmp(line.data(), "status") == 0) status();
                else if (std::strcmp(line.data(), "scan") == 0) {
                    if (state.load() > 0 && state.load() != 2 && request.load() == Request::None)
                        request.store(Request::Scan);
                    else std::printf("Not ready or request busy.\r\n");
                } else if (std::strcmp(line.data(), "networks") == 0) {
                    WifiScan scan;
                    wifi_scan_snapshot(&scan);
                    std::printf("Scan: %s, %u results, %u omitted\r\n", scan.running ? "running" : "complete", scan.count, scan.omitted);
                    for (unsigned i{}; i < scan.count; ++i)
                        std::printf("%-32s %4d dBm ch %u\r\n", scan.entries[i].ssid, scan.entries[i].rssi, scan.entries[i].channel);
                } else if (std::strcmp(line.data(), "reconnect") == 0) {
                    if ((state.load() == 1 || state.load() == 3) && request.load() == Request::None)
                        request.store(Request::Reconnect);
                    else std::printf("Not ready or request busy.\r\n");
                }
                else if (std::strcmp(line.data(), "start") == 0 || std::strcmp(line.data(), "stop") == 0 ||
                         std::strcmp(line.data(), "echo") == 0) {
                    if (state.load() != 3 || request.load() != Request::None) std::printf("Not connected or request busy.\r\n");
                    else request.store(line[0] == 'e' ? Request::Echo : (line[2] == 'a' ? Request::Start : Request::Stop));
                }
                else if (std::strcmp(line.data(), "disconnect") == 0) {
                    if (state.load() == 3 && request.load() == Request::None) request.store(Request::Disconnect);
                } else std::printf("wifi | scan | networks | status | start | stop | echo | disconnect | reconnect | help\r\n");
                line.fill(0);
                length = 0;
                overflow = false;
                if (input == Input::Command) std::printf("> ");
            } else if (c == '\b' || c == 127) {
                if (length) { line[--length] = 0; if (input != Input::Password) std::printf("\b \b"); }
            } else if (static_cast<unsigned char>(c) >= 32) {
                if (length + 1 < line.size()) line[length++] = c;
                else overflow = true;
                if (input != Input::Password) std::putchar(c);
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
    }
}
