#include "storage_startup_checks.hpp"
#include "hal/panic.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Fixture
    {
        std::string path;
        std::string kind;
        std::size_t bytes;
        bool owned{};

        auto prepare() -> bool
        {
            if (std::filesystem::exists(path)) return false;
            if (kind == "absent") {
                owned = ::mkdir(path.c_str(), 0700) == 0;
                return owned;
            }
            if (kind == "valid") {
                owned = true; // Simulator creates only this initially absent path.
                return true;
            }
            auto* output{ std::fopen(path.c_str(), "wbx") };
            if (!output) return false;
            owned = true;
            std::array<unsigned char, 4096> block{};
            block.fill(0xA5U);
            bool ok{ true };
            for (std::size_t offset{}; offset < bytes; offset += block.size())
                ok = std::fwrite(block.data(), 1, block.size(), output) == block.size() && ok;
            return std::fclose(output) == 0 && ok;
        }

        auto unchanged() const -> bool
        {
            if (kind != "corrupt") return true;
            std::ifstream input{ path, std::ios::binary };
            std::array<unsigned char, 4096> block{};
            std::size_t count{};
            while (input.read(reinterpret_cast<char*>(block.data()), block.size())) {
                count += block.size();
                for (const auto byte : block) if (byte != 0xA5U) return false;
            }
            return input.eof() && input.gcount() == 0 && count == bytes;
        }

        void cleanup()
        {
            if (owned) {
                if (kind == "absent") static_cast<void>(::rmdir(path.c_str()));
                else static_cast<void>(::unlink(path.c_str()));
                owned = false;
            }
        }
        ~Fixture() { cleanup(); }
    };
    std::vector<Fixture> fixtures;
    std::string console;
    bool fail_output{};
    bool continued{};
    unsigned output_calls{};
    bool expected_flash{}, expected_sd{};
    auto intact() -> bool
    {
        for (const auto& fixture : fixtures) if (!fixture.unchanged()) return false;
        return true;
    }
}

extern "C" int __io_putchar(int character)
{
    ++output_calls;
    if (fail_output) return -1;
    console.push_back(static_cast<char>(character));
    const char byte{ static_cast<char>(character) };
    return ::write(STDOUT_FILENO, &byte, 1) == 1 ? character : -1;
}
extern "C" int __io_getchar()
{
    static unsigned count{};
    return count++ == 0 ? 'x' : '\n';
}

extern "C" [[noreturn]] void hal_panic_handler(const HalPanicInfo* info) noexcept
{
    const bool expected{ info && info->message &&
        std::strcmp(info->message, "Persistent storage required but no volume is available") == 0 &&
        !expected_flash && !expected_sd && !continued && !runtime::filex::initialized() && intact() };
    std::printf("[storage-probe] panic: %s\n", info && info->message ? info->message : "unknown");
    for (auto& fixture : fixtures) fixture.cleanup();
    std::exit(expected ? 42 : 43);
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string scenario{ argv[1] };
    std::string flash{ "valid" }, sd{ "valid" };
    if (scenario == "flash") sd = "absent";
    else if (scenario == "sd") flash = "absent";
    else if (scenario == "none" || scenario == "warning-failure") flash = sd = "absent";
    else if (scenario == "corrupt") flash = sd = "corrupt";
    else if (scenario == "flash-corrupt-sd") sd = "corrupt";
    else if (scenario == "sd-corrupt-flash") flash = "corrupt";
    else if (scenario != "both") return 2;
    expected_flash = flash == "valid";
    expected_sd = sd == "valid";
    fixtures.reserve(2);
    const std::string prefix{ "/tmp/nucleo-h753zi-" + std::to_string(::getpid()) };
    fixtures.push_back({ prefix + "-flash-levelx.img", flash, 16U * 1024U * 1024U });
    fixtures.push_back({ prefix + "-sd-filex.img", sd, 64U * 1024U * 1024U });
    for (auto& fixture : fixtures) if (!fixture.prepare()) return 3;
    fail_output = scenario == "warning-failure";
    runtime_filex_initialize();
    continued = true;
    fail_output = false;
    const unsigned initialization_output_calls{ output_calls };
    const int check{ runtime::tests::check_storage_startup_adapters(expected_flash, expected_sd) };
    if (check) { std::printf("FAIL: adapter check line %d\n", check); return 4; }
    if (!intact()) return 5;
    const bool unavailable{ !expected_flash && !expected_sd };
    constexpr char warning[]{ "[storage] unavailable; file access disabled\n" };
    const auto first{ console.find(warning) };
    if (scenario == "warning-failure") {
        if (initialization_output_calls != 1U) return 6;
    }
    else if ((first != std::string::npos) != unavailable ||
             (first != std::string::npos && console.find(warning, first + 1) != std::string::npos)) return 6;
    std::array<char, 4> input{};
    if (_read(0, input.data(), input.size()) != 2 || std::strcmp(input.data(), "x\n") != 0) return 7;
    std::atomic_uint progress{};
    {
        std::jthread worker{ [&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                ++progress;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        } };
        for (unsigned i{}; i < 1000U; ++i) {
            runtime_filex_initialize();
            if (unavailable) {
                errno = 0;
                if (_open("/flash/rejected", O_RDONLY) != -1 || errno != ENODEV) return 8;
            }
            if (i % 100U == 0) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    if (!progress) return 9;
    // Linux application streams continue to use the host filesystem.
    const std::string host_path{ prefix + "-host.txt" };
    { std::ofstream host{ host_path }; host << "host filesystem remains usable"; if (!host) return 10; }
    std::ifstream host{ host_path };
    std::string line;
    std::getline(host, line);
    static_cast<void>(::unlink(host_path.c_str()));
    if (line != "host filesystem remains usable") return 10;
    std::printf("PASS: storage startup %s; application continued; worker=%u\n", scenario.c_str(), progress.load());
    return 0;
}
