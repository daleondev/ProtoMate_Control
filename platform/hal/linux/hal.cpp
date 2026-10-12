#include "hal/hal.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    auto panic_write(const char* text) noexcept -> void
    {
        if (text == nullptr) {
            return;
        }

        std::size_t length{};
        while (text[length] != '\0') {
            ++length;
        }
        while (length != 0U) {
            const ssize_t written{ ::write(STDERR_FILENO, text, length) };
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (written == 0) {
                return;
            }
            text += written;
            length -= static_cast<std::size_t>(written);
        }
    }

    auto panic_write_line(std::uint32_t line) noexcept -> void
    {
        constexpr auto decimal_base{ 10U };
        std::array<char, std::numeric_limits<std::uint32_t>::digits10 + 1U> digits{};
        std::size_t count{};
        while (true) {
            digits[count++] = static_cast<char>('0' + (line % decimal_base));
            line /= decimal_base;
            if (line == 0U) {
                break;
            }
        }
        while (count != 0U) {
            const std::array character{ digits[--count], '\0' };
            panic_write(character.data());
        }
    }
}

extern "C" [[gnu::weak, gnu::noinline, noreturn]] void hal_panic_handler(const HalPanicInfo* info) noexcept
{
    panic_write("[hal][panic] ");
    panic_write(info != nullptr && info->message != nullptr ? info->message : "fatal error");
    if (info != nullptr && info->detail != nullptr) {
        panic_write(": ");
        panic_write(info->detail);
    }
    if (info != nullptr && info->file != nullptr) {
        panic_write("\n  at ");
        panic_write(info->file);
        if (info->line != 0U) {
            panic_write(":");
            panic_write_line(info->line);
        }
        if (info->function != nullptr) {
            panic_write(" (");
            panic_write(info->function);
            panic_write(")");
        }
    }
    panic_write("\n");
    std::abort();
}
