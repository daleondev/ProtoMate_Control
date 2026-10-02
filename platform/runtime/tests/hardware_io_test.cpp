#include "hal/hal.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

extern "C" {
// Set mode at select(), after reset and runtime initialization: 0 prepares,
// 1 verifies without rewriting. These symbols belong only to this test image.
volatile std::uint32_t hardware_io_test_mode{};
volatile std::uint32_t hardware_io_test_status{};
volatile std::uint32_t hardware_io_test_phase{};
[[gnu::noinline, gnu::used]] void hardware_io_test_select() { asm volatile("" ::: "memory"); }
[[gnu::noinline, gnu::used]] void hardware_io_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    namespace fs = std::filesystem;
    constexpr char CONTENT[]{ "Nucleo reset persistence: explicitly closed file\n" };

    auto timestamp() -> fs::file_time_type
    {
        using namespace std::chrono;
        return fs::file_time_type::clock::from_sys(sys_days{ year{ 2024 } / January / 2 } + 3h + 4min + 6s);
    }

    auto serial_roundtrip() -> bool
    {
        constexpr std::array labels{ "CR", "LF", "CRLF", "AFTER" };
        for (const char* label : labels) {
            std::printf("[io-input-%s]\n", label);
            if (std::fflush(stdout) != 0) {
                return false;
            }
            std::array<char, 32U> line{};
            const std::string expected{ std::string{ "token-" } + label + '\n' };
            if (std::fgets(line.data(), static_cast<int>(line.size()), stdin) == nullptr ||
                line.data() != expected) {
                return false;
            }
            std::printf("[io-echo-%s]\n", label);
            if (std::fflush(stdout) != 0) {
                return false;
            }
        }
        return true;
    }

    auto prepare(const char* volume) -> bool
    {
        const fs::path parent{ std::string{ volume } + "/nucleo-io-test.prepare" };
        const fs::path committed{ std::string{ volume } + "/nucleo-io-test.committed" };
        if (fs::exists(parent) || fs::exists(committed) || !fs::create_directory(parent)) {
            return false;
        }
        const fs::path temporary{ parent / "temporary.bin" };
        std::FILE* file{ std::fopen(temporary.c_str(), "wb") };
        if (file == nullptr) {
            return false;
        }
        const bool written{ std::fwrite(CONTENT, 1U, sizeof(CONTENT) - 1U, file) == sizeof(CONTENT) - 1U };
        const bool closed{ std::fclose(file) == 0 };
        if (!written || !closed) {
            return false;
        }
        const fs::path payload{ parent / "payload.bin" };
        fs::rename(temporary, payload);
        fs::last_write_time(payload, timestamp());
        const fs::path removed{ parent / "removed" };
        if (!fs::create_directory(removed) || !fs::remove(removed)) {
            return false;
        }
        fs::last_write_time(parent, timestamp());
        fs::rename(parent, committed);
        return true;
    }

    auto verify(const char* volume) -> bool
    {
        const fs::path committed{ std::string{ volume } + "/nucleo-io-test.committed" };
        const fs::path payload{ committed / "payload.bin" };
        if (fs::exists(std::string{ volume } + "/nucleo-io-test.prepare") || !fs::is_directory(committed) ||
            fs::exists(committed / "temporary.bin") || fs::exists(committed / "removed") ||
            fs::file_size(payload) != sizeof(CONTENT) - 1U || fs::last_write_time(payload) != timestamp() ||
            fs::last_write_time(committed) != timestamp()) {
            return false;
        }
        std::FILE* file{ std::fopen(payload.c_str(), "rb") };
        if (file == nullptr) {
            return false;
        }
        std::array<char, sizeof(CONTENT)> contents{};
        const bool read{ std::fread(contents.data(), 1U, contents.size(), file) == sizeof(CONTENT) - 1U };
        const bool closed{ std::fclose(file) == 0 };
        return read && closed && std::strcmp(contents.data(), CONTENT) == 0;
    }
}

int main()
{
    hardware_io_test_select();
    bool passed{};
    try {
        if (hardware_io_test_mode == 0U) {
            hardware_io_test_phase = 1U;
            if (serial_roundtrip()) {
                hardware_io_test_phase = 2U;
                passed = prepare("/flash") && prepare("/sd");
            }
        }
        else if (hardware_io_test_mode == 1U) {
            hardware_io_test_phase = 3U;
            passed = verify("/flash") && verify("/sd");
            if (passed) {
                passed = fs::remove_all("/flash/nucleo-io-test.committed") == 2U &&
                         fs::remove_all("/sd/nucleo-io-test.committed") == 2U;
            }
        }
    } catch (const std::exception& error) {
        std::printf("[io-test] %s\n", error.what());
    } catch (...) {
        std::puts("[io-test] unknown exception");
    }
    hardware_io_test_status = passed ? 0x600D600DU : 0xBAD00000U | hardware_io_test_phase;
    std::printf("[io-test] %s phase %lu\n",
                passed ? "PASS" : "FAIL",
                static_cast<unsigned long>(hardware_io_test_phase));
    std::fflush(stdout);
    hardware_io_test_complete();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds{ 1 });
    }
}
