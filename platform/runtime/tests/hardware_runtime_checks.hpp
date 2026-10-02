#pragma once

#include "runtime/thread.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace runtime::hardware_checks
{
    inline thread_local unsigned int late_startup_tls_value{};
    inline unsigned int* late_startup_tls_address{};

    // Call from runtime_application_define(), after global constructors but
    // before the application's ThreadX thread starts.
    inline void capture_late_startup_tls()
    {
        late_startup_tls_value = 0x71A5U;
        late_startup_tls_address = &late_startup_tls_value;
    }

    [[nodiscard]] inline bool late_startup_tls_is_preserved()
    {
        return late_startup_tls_address == &late_startup_tls_value && late_startup_tls_value == 0x71A5U;
    }

    [[nodiscard]] inline bool non_native_atomic_width()
    {
        using Record = std::array<std::uint8_t, 3U>;
        constexpr Record initial{ 1U, 2U, 3U };
        constexpr Record replacement{ 4U, 5U, 6U };
        std::atomic<Record> value{ initial };
        Record expected{ initial };
        return !value.is_lock_free() && value.compare_exchange_strong(expected, replacement) &&
               value.load() == replacement;
    }

    [[nodiscard]] inline bool thread_names()
    {
        constexpr std::array name_storage{ 'i', 'o', '_', 'w', 'o', 'r', 'k', 'e', 'r', 'X' };
        const std::array<std::pair<std::string_view, std::string_view>, 3U> cases{ {
          { std::string_view{ name_storage.data(), name_storage.size() - 1U }, "IoWorker Thread" },
          { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" },
          { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Unknown Thread" },
        } };
        for (const auto& [requested, expected] : cases) {
            std::string observed;
            auto worker = runtime::thread::create({ .name = requested },
                                                  [&] { observed = tx_thread_identify()->tx_thread_name; });
            worker.join();
            if (observed != expected) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool tls_destructor_can_join()
    {
        struct State
        {
            bool child_ran{};
            bool destructor_finished{};
        } state;
        struct Probe
        {
            State* state{};

            ~Probe()
            {
                if (state != nullptr) {
                    std::thread child{ [this] { state->child_ran = true; } };
                    child.join();
                    state->destructor_finished = true;
                }
            }
        };

        std::thread worker{ [&] {
            thread_local Probe probe;
            probe.state = &state;
        } };
        worker.join();
        return state.child_ran && state.destructor_finished;
    }

    [[nodiscard]] inline bool static_initializer_can_join()
    {
        static const int outer{ [] {
            int result{};
            std::thread child{ [&] {
                static const int inner{ [] { return tx_thread_identify() != nullptr ? 42 : 0; }() };
                result = inner;
            } };
            child.join();
            return result;
        }() };
        return outer == 42;
    }

    [[nodiscard]] inline bool native_thread_reset_reinitializes_runtime()
    {
        struct State
        {
            TX_THREAD thread{};
            alignas(8) std::array<std::byte, 4096U> stack{};
            detail::KeyHandle key{};
            std::atomic_uint destructors{};
            unsigned int runs{};
            bool initialized{ true };
        };
        struct Probe
        {
            unsigned int value{ 17U };
            std::atomic_uint* destructors{};

            ~Probe()
            {
                if (destructors != nullptr) {
                    destructors->fetch_add(1U, std::memory_order_relaxed);
                }
            }
        };

        auto state{ std::make_unique<State>() };
        if (detail::key_create(&state->key, nullptr) != 0) {
            return false;
        }
        CHAR name[]{ "TLS reset probe" };
        const UINT create_status{ tx_thread_create(&state->thread,
                                                   name,
                                                   [](ULONG input) {
            auto& current{ *reinterpret_cast<State*>(input) };
            thread_local Probe probe;
            current.initialized = current.initialized && probe.value == 17U && errno == 0 &&
                                  detail::key_get(current.key) == nullptr;
            ++current.runs;
            probe.value = 99U;
            probe.destructors = &current.destructors;
            errno = EDOM;
            current.initialized = current.initialized && detail::key_set(current.key, &current) == 0;
        },
                                                   reinterpret_cast<ULONG>(state.get()),
                                                   state->stack.data(),
                                                   state->stack.size(),
                                                   16U,
                                                   16U,
                                                   TX_NO_TIME_SLICE,
                                                   TX_AUTO_START) };

        const auto wait_until_completed = [&] {
            const auto deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 1 } };
            while (std::chrono::steady_clock::now() < deadline) {
                UINT status{};
                if (tx_thread_info_get(&state->thread,
                                       nullptr,
                                       &status,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr) != TX_SUCCESS) {
                    return false;
                }
                if (status == TX_COMPLETED) {
                    return true;
                }
                if (tx_thread_sleep(1U) != TX_SUCCESS) {
                    return false;
                }
            }
            return false;
        };

        bool success{ create_status == TX_SUCCESS && wait_until_completed() &&
                      tx_thread_reset(&state->thread) == TX_SUCCESS &&
                      tx_thread_resume(&state->thread) == TX_SUCCESS && wait_until_completed() };
        if (create_status == TX_SUCCESS) {
            const UINT terminate_status{ tx_thread_terminate(&state->thread) };
            const UINT delete_status{ tx_thread_delete(&state->thread) };
            success = success && terminate_status == TX_SUCCESS && delete_status == TX_SUCCESS;
        }
        const int key_status{ detail::key_delete(state->key) };
        return success && key_status == 0 && state->initialized && state->runs == 2U &&
               state->destructors.load(std::memory_order_relaxed) == 2U;
    }
}
