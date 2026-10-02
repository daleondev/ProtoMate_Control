#include "hal/hal.hpp"
#include "hal/linux/console.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    auto print_message(std::string_view message) -> void
    {
        std::string line{ "[sim][hal] " };
        line.append(message);
        line.push_back('\n');
        linux_console::publish(linux_console::Channel::hal, line);
    }

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
        char digits[10]{};
        std::size_t count{};
        do {
            digits[count++] = static_cast<char>('0' + line % 10U);
            line /= 10U;
        } while (line != 0U);
        while (count != 0U) {
            const char character[2]{ digits[--count], '\0' };
            panic_write(character);
        }
    }
}

extern "C" {

HAL_StatusTypeDef HAL_Init()
{
    print_message("HAL_Init");
    return HAL_OK;
}

void SystemClock_Config() { print_message("SystemClock_Config"); }

void MPU_Config_User() { print_message("MPU_Config_User"); }

void SCB_EnableICache() { print_message("SCB_EnableICache"); }

void SCB_EnableDCache() { print_message("SCB_EnableDCache"); }

void MX_GPIO_Init() { print_message("MX_GPIO_Init"); }

void MX_ETH_Init() { print_message("MX_ETH_Init"); }

void MX_RTC_Init() { print_message("MX_RTC_Init"); }

void MX_TIM2_Init() { print_message("MX_TIM2_Init"); }

void MX_RNG_Init() { print_message("MX_RNG_Init"); }

int32_t BSP_COM_Init(COM_TypeDef com, COM_InitTypeDef* com_init)
{
    const auto baud_rate{ com_init != nullptr ? com_init->BaudRate : 0U };
    print_message(std::string{ "BSP_COM_Init(com=" } + std::to_string(static_cast<int>(com)) +
                  ", baud=" + std::to_string(baud_rate) + ')');
    return BSP_ERROR_NONE;
}

}

extern "C" [[gnu::weak, gnu::noinline, noreturn]] void hal_panic_handler(
  const HalPanicInfo* info) noexcept
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
