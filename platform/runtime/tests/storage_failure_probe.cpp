#include "libc/storage_media.hpp"

#include <array>
#include <cstdio>
#include <fx_api.h>
#include <string>
#include <unistd.h>

namespace
{
    struct Fixture
    {
        std::string path;
        bool owned{};
        ~Fixture()
        {
            if (owned) {
                static_cast<void>(::unlink(path.c_str()));
            }
        }

        auto create(std::size_t bytes) -> bool
        {
            std::FILE* file{ std::fopen(path.c_str(), "wbx") };
            if (file == nullptr) {
                return false;
            }
            owned = true;
            std::array<unsigned char, 4096U> block{};
            block.fill(0xA5U);
            bool written{ true };
            for (std::size_t offset{}; offset < bytes; offset += block.size()) {
                if (std::fwrite(block.data(), 1U, block.size(), file) != block.size()) {
                    written = false;
                    break;
                }
            }
            const bool closed{ std::fclose(file) == 0 };
            return written && closed;
        }

        auto unchanged(std::size_t expected_size) -> bool
        {
            std::FILE* file{ std::fopen(path.c_str(), "rb") };
            if (file == nullptr) {
                return false;
            }
            std::array<unsigned char, 4096U> block{};
            std::size_t bytes{};
            bool valid{ true };
            while (const auto read = std::fread(block.data(), 1U, block.size(), file)) {
                bytes += read;
                for (std::size_t i{}; i < read; ++i) {
                    valid = valid && block[i] == 0xA5U;
                }
            }
            valid = valid && std::ferror(file) == 0 && bytes == expected_size;
            const bool closed{ std::fclose(file) == 0 };
            return valid && closed;
        }
    };
}

int main()
{
    const std::string prefix{ "/tmp/nucleo-h753zi-" + std::to_string(::getpid()) };
    Fixture flash{ prefix + "-flash-levelx.img" };
    Fixture sd{ prefix + "-sd-filex.img" };
    constexpr std::size_t FLASH_BYTES{ 16U * 1024U * 1024U };
    constexpr std::size_t SD_BYTES{ 64U * 1024U * 1024U };
    if (!flash.create(FLASH_BYTES) || !sd.create(SD_BYTES)) {
        return 1;
    }
    fx_system_initialize();
    const bool mounted{ runtime::storage::initialize() };
    const auto& diagnostics{ runtime::storage::diagnostics() };
    const bool passed{ !mounted && !diagnostics.flash_mounted && !diagnostics.sd_mounted &&
                       !diagnostics.flash_was_formatted && flash.unchanged(FLASH_BYTES) &&
                       sd.unchanged(SD_BYTES) };
    std::puts(passed ? "PASS: corrupt images preserved" : "FAIL: corrupt media policy");
    return passed ? 0 : 1;
}
