#include "storage_startup_checks.hpp"
#include "hal/board/board.hpp"
#include "hal/hal.hpp"

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

extern "C" {
volatile std::uint32_t hardware_storage_startup_test_volume_mask{ 3U };
volatile std::uint32_t hardware_storage_startup_test_expected_mask{ 3U };
volatile std::uint32_t hardware_storage_startup_test_application_entered{};
volatile std::uint32_t hardware_storage_startup_test_status{};
volatile std::uint32_t hardware_storage_startup_test_phase{};
volatile std::uint32_t hardware_storage_startup_test_failure_line{};
volatile std::uint32_t hardware_storage_startup_test_heartbeat{};
[[gnu::noinline, gnu::used]] void hardware_storage_startup_test_select() { asm volatile("" ::: "memory"); }
[[gnu::noinline, gnu::used]] void hardware_storage_startup_test_entered() { asm volatile("" ::: "memory"); }
[[gnu::noinline, gnu::used]] void hardware_storage_startup_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    auto standard_library_checks(bool flash, bool sd) -> int
    {
        for (unsigned volume{}; volume < 2; ++volume) {
            const bool mounted{ volume == 0U ? flash : sd };
            const fs::path mount{ volume == 0U ? "/flash" : "/sd" };
            const auto file{ mount / "nucleo-storage-startup-stdio.tmp" };
            const auto stream_file{ mount / "nucleo-storage-startup-stream.tmp" };
            const auto directory{ mount / "nucleo-storage-startup-stdlib.dir" };
            std::error_code error;
            if (!mounted) {
                errno = 0;
                auto* input{ std::fopen(file.c_str(), "rb") };
                STORAGE_CHECK(input == nullptr && errno == ENODEV);
                std::ofstream output{ stream_file };
                STORAGE_CHECK(output.fail());
                STORAGE_CHECK(!fs::exists(file, error) && error.value() == ENODEV);
                static_cast<void>(fs::space(mount, error));
                STORAGE_CHECK(error.value() == ENODEV);
                fs::directory_iterator iterator{ mount, error };
                STORAGE_CHECK(error.value() == ENODEV);
                errno = 0;
                STORAGE_CHECK(::opendir(mount.c_str()) == nullptr && errno == ENODEV);
                continue;
            }
            // Every fixture is exclusive or checked absent before creation.
            auto* output{ std::fopen(file.c_str(), "wbx") };
            STORAGE_CHECK(output != nullptr);
            STORAGE_CHECK(std::fputs("closed data", output) >= 0);
            STORAGE_CHECK(std::fclose(output) == 0);
            auto* input{ std::fopen(file.c_str(), "rb") };
            STORAGE_CHECK(input != nullptr);
            std::array<char, 16> read{};
            STORAGE_CHECK(std::fread(read.data(), 1, 11, input) == 11);
            STORAGE_CHECK(std::fclose(input) == 0 && std::strcmp(read.data(), "closed data") == 0);
            STORAGE_CHECK(fs::remove(file, error) && !error);
            STORAGE_CHECK(!fs::exists(stream_file, error) && !error);
            { std::ofstream stream{ stream_file }; stream << "stream data"; stream.close(); STORAGE_CHECK(!stream.fail()); }
            { std::ifstream stream{ stream_file }; std::string line; std::getline(stream, line); STORAGE_CHECK(line == "stream data"); }
            STORAGE_CHECK(fs::remove(stream_file, error) && !error);
            STORAGE_CHECK(fs::create_directory(directory, error) && !error);
            fs::directory_iterator iterator{ directory, error };
            STORAGE_CHECK(!error && iterator == fs::directory_iterator{});
            STORAGE_CHECK(fs::remove(directory, error) && !error);
        }
        if (!flash && !sd) {
            std::error_code error;
            static_cast<void>(fs::current_path(error));
            STORAGE_CHECK(error.value() == ENODEV);
            fs::directory_iterator iterator{ "/", error };
            STORAGE_CHECK(error.value() == ENODEV);
        }
        return 0;
    }

    auto echo(const char* marker, const char* expected, const char* result) -> bool
    {
        std::puts(marker);
        static_cast<void>(std::fflush(stdout));
        std::array<char, 64> input{};
        if (!std::fgets(input.data(), input.size(), stdin) || std::strcmp(input.data(), expected) != 0)
            return false;
        std::puts(result);
        static_cast<void>(std::fflush(stdout));
        return true;
    }
}

int main()
{
    hardware_storage_startup_test_application_entered = 1U;
    hardware_storage_startup_test_entered();
    std::puts("[storage-startup] application entered");
    static_cast<void>(std::fflush(stdout));
    const unsigned mask{ hardware_storage_startup_test_expected_mask };
    const bool flash{ (mask & 1U) != 0U }, sd{ (mask & 2U) != 0U };
    const auto led{ hal::board::createLed(hal::board::LedId::Green) };
    int failure{ led ? 0 : __LINE__ };
    std::atomic_uint heartbeat{};
    {
        std::jthread worker{ [&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (led) led->toggle();
                const auto count{ heartbeat.fetch_add(1U) + 1U };
                hardware_storage_startup_test_heartbeat = count;
                std::this_thread::sleep_for(100ms);
            }
        } };
        hardware_storage_startup_test_phase = 1U;
        if (!failure) failure = runtime::tests::check_storage_startup_adapters(flash, sd);
        hardware_storage_startup_test_phase = 2U;
        if (!failure) failure = standard_library_checks(flash, sd);
        hardware_storage_startup_test_phase = 3U;
        if (!failure && !echo("[storage-input]", "storage-echo\n", "[storage-echo]")) failure = __LINE__;
        const auto started{ std::chrono::steady_clock::now() };
        for (unsigned i{}; i < 1000U && !failure; ++i) {
            runtime_filex_initialize();
            if (!flash && !sd) {
                errno = 0;
                if (_open("/flash/rejected", O_RDONLY) != -1 || errno != ENODEV) failure = __LINE__;
            }
        }
        while (!failure && std::chrono::steady_clock::now() - started < 10s)
            std::this_thread::sleep_for(100ms);
        if (!failure && !echo("[storage-input-after]", "storage-after\n", "[storage-echo-after]")) failure = __LINE__;
        if (!failure && heartbeat.load() < 80U) failure = __LINE__;
    }
    hardware_storage_startup_test_failure_line = failure;
    hardware_storage_startup_test_status = failure ? 0xBAD00000U : 0x600D600DU;
    std::printf("[storage-startup] %s line=%d heartbeat=%u\n", failure ? "FAIL" : "PASS", failure, heartbeat.load());
    static_cast<void>(std::fflush(stdout));
    hardware_storage_startup_test_complete();
    while (true) std::this_thread::sleep_for(1s);
}
