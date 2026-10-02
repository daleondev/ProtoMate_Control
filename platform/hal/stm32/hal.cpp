#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <tx_api.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace
{
    constexpr std::size_t CONSOLE_INPUT_BUFFER_SIZE{ 256U };
    constexpr std::uint32_t CONSOLE_UART_INTERRUPT_PRIORITY{ 5U };
    constexpr std::uint32_t RED_LED_PIN_NUMBER{ std::countr_zero(static_cast<std::uint32_t>(LED3_PIN)) };
    constexpr std::uint32_t GPIO_MODE_BITS{ 2U };
    constexpr std::uint32_t UART_SPIN_LIMIT{ 1'000'000U };

    static_assert(std::has_single_bit(CONSOLE_INPUT_BUFFER_SIZE));
    static_assert(std::atomic_size_t::is_always_lock_free);

    // The USART ISR produces bytes and the stdin thread consumes them.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    TX_SEMAPHORE console_input_available{};
    TX_MUTEX console_input_mutex{};
    TX_MUTEX console_output_mutex{};
    std::array<std::uint8_t, CONSOLE_INPUT_BUFFER_SIZE> console_input_buffer{};
    std::atomic_size_t console_input_read{};
    std::atomic_size_t console_input_write{};
    std::array<CHAR, sizeof("COM1 input")> console_input_semaphore_name{ "COM1 input" };
    CHAR console_input_mutex_name[]{ "COM1 reader" };
    CHAR console_output_mutex_name[]{ "COM1 writer" };
    bool console_input_ready{};
    bool discard_line_feed{};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    [[nodiscard]] auto pop_console_character(std::uint8_t& character) noexcept -> bool
    {
        const std::size_t read{ console_input_read.load(std::memory_order_relaxed) };
        if (read == console_input_write.load(std::memory_order_acquire)) {
            return false;
        }

        character = console_input_buffer[read];
        console_input_read.store((read + 1U) & (CONSOLE_INPUT_BUFFER_SIZE - 1U), std::memory_order_release);
        return true;
    }

    [[nodiscard]] auto initialize_console() noexcept -> bool
    {
        // Raw read/write calls and separate FILE streams do not share Newlib's
        // stream lock. Create the RTOS objects exactly once without scheduling
        // another thread midway through initialization.
        const hal::stm32::InterruptGuard guard;
        if (console_input_ready) {
            return true;
        }

        if (tx_mutex_create(&console_input_mutex, console_input_mutex_name, TX_INHERIT) != TX_SUCCESS) {
            return false;
        }
        if (tx_mutex_create(&console_output_mutex, console_output_mutex_name, TX_INHERIT) != TX_SUCCESS) {
            static_cast<void>(tx_mutex_delete(&console_input_mutex));
            return false;
        }
        if (tx_semaphore_create(&console_input_available, console_input_semaphore_name.data(), 0U) !=
            TX_SUCCESS) {
            static_cast<void>(tx_mutex_delete(&console_output_mutex));
            static_cast<void>(tx_mutex_delete(&console_input_mutex));
            return false;
        }

        console_input_ready = true;
        HAL_NVIC_SetPriority(USART3_IRQn, CONSOLE_UART_INTERRUPT_PRIORITY, 0U);
        HAL_NVIC_EnableIRQ(USART3_IRQn);
        __HAL_UART_ENABLE_IT(&hcom_uart[COM1], UART_IT_RXNE);
        __HAL_UART_ENABLE_IT(&hcom_uart[COM1], UART_IT_IDLE);
        return true;
    }

    [[nodiscard]] auto receive_console_character() noexcept -> int
    {
        if (tx_thread_identify() == nullptr) {
            std::uint8_t character{};
            return HAL_UART_Receive(&hcom_uart[COM1], &character, 1U, HAL_MAX_DELAY) == HAL_OK
                     ? static_cast<int>(character)
                     : -1;
        }

        std::uint8_t character{};
        while (!pop_console_character(character)) {
            if (tx_semaphore_get(&console_input_available, TX_WAIT_FOREVER) != TX_SUCCESS) {
                return -1;
            }
        }
        return static_cast<int>(character);
    }

    [[nodiscard]] auto read_console_character() noexcept -> int
    {
        while (true) {
            const int character{ receive_console_character() };
            if (character < 0) {
                return -1;
            }
            if (discard_line_feed) {
                discard_line_feed = false;
                if (character == '\n') {
                    continue;
                }
            }
            if (character == '\r') {
                discard_line_feed = true;
                return '\n';
            }
            return character;
        }
    }

    auto panic_uart_write_character(char character) noexcept -> void
    {
        constexpr std::uint32_t enabled_mask{ USART_CR1_UE | USART_CR1_TE };
        if ((COM1_UART->CR1 & enabled_mask) != enabled_mask) {
            return;
        }

        std::uint32_t remaining{ UART_SPIN_LIMIT };
        while ((COM1_UART->ISR & USART_ISR_TXE_TXFNF) == 0U && remaining != 0U) {
            --remaining;
        }
        if (remaining != 0U) {
            COM1_UART->TDR = static_cast<std::uint8_t>(character);
        }
    }

    auto panic_write(const char* text) noexcept -> void
    {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0') {
            panic_uart_write_character(*text++);
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
            panic_uart_write_character(digits[--count]);
        }
    }

    auto configure_red_led() noexcept -> void
    {
        LED3_GPIO_CLK_ENABLE();
        __DSB();

        constexpr std::uint32_t mode_shift{ RED_LED_PIN_NUMBER * GPIO_MODE_BITS };
        constexpr std::uint32_t mode_mask{ 0x3U << mode_shift };
        LED3_GPIO_PORT->MODER = (LED3_GPIO_PORT->MODER & ~mode_mask) | (0x1U << mode_shift);
        LED3_GPIO_PORT->OTYPER &= ~LED3_PIN;
        LED3_GPIO_PORT->OSPEEDR &= ~mode_mask;
        LED3_GPIO_PORT->PUPDR &= ~mode_mask;
        LED3_GPIO_PORT->BSRR = static_cast<std::uint32_t>(LED3_PIN) << 16U;
    }

    auto set_red_led(bool on) noexcept -> void
    {
        LED3_GPIO_PORT->BSRR = on ? LED3_PIN : static_cast<std::uint32_t>(LED3_PIN) << 16U;
    }

    auto panic_delay() noexcept -> void
    {
        const std::uint32_t iterations{ SystemCoreClock >= 1'000'000U ? SystemCoreClock / 32U : 2'000'000U };
        for (std::uint32_t iteration{}; iteration < iterations; ++iteration) {
            __NOP();
        }
    }

    auto write_panic_info(const HalPanicInfo* info) noexcept -> void
    {
        panic_write("\r\n[hal][panic] ");
        panic_write(info != nullptr && info->message != nullptr ? info->message : "fatal error");
        if (info != nullptr && info->detail != nullptr) {
            panic_write(": ");
            panic_write(info->detail);
        }
        if (info != nullptr && info->file != nullptr) {
            panic_write("\r\n  at ");
            panic_write(info->file);
            if (info->line != 0U) {
                panic_uart_write_character(':');
                panic_write_line(info->line);
            }
            if (info->function != nullptr) {
                panic_write(" (");
                panic_write(info->function);
                panic_uart_write_character(')');
            }
        }
        panic_write("\r\n");
    }
}

extern "C" int __io_getchar() noexcept
{
    if (__get_IPSR() != 0U || hcom_uart[COM1].Instance == nullptr) {
        return -1;
    }
    if (tx_thread_identify() == nullptr) {
        return read_console_character();
    }
    if (!initialize_console() || tx_mutex_get(&console_input_mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
        return -1;
    }
    const int character{ read_console_character() };
    static_cast<void>(tx_mutex_put(&console_input_mutex));
    return character;
}

extern "C" int __io_putchar(int character) noexcept
{
    if (__get_IPSR() != 0U || hcom_uart[COM1].Instance == nullptr) {
        return -1;
    }
    const bool threaded{ tx_thread_identify() != nullptr };
    if (threaded &&
        (!initialize_console() || tx_mutex_get(&console_output_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)) {
        return -1;
    }
    const std::uint8_t byte{ static_cast<std::uint8_t>(character) };
    const auto status{ HAL_UART_Transmit(&hcom_uart[COM1], &byte, 1U, COM_POLL_TIMEOUT) };
    if (threaded) {
        static_cast<void>(tx_mutex_put(&console_output_mutex));
    }
    // The BSP implementation reports success even when a preempting writer
    // gets HAL_BUSY. Serialize access and propagate actual transmission errors.
    return status == HAL_OK ? static_cast<int>(byte) : -1;
}

extern "C" void USART3_IRQHandler() noexcept
{
    bool notify_reader{};
    while ((COM1_UART->ISR & USART_ISR_RXNE_RXFNE) != 0U) {
        const std::uint8_t character{ static_cast<std::uint8_t>(COM1_UART->RDR) };
        const std::size_t write{ console_input_write.load(std::memory_order_relaxed) };
        const std::size_t next{ (write + 1U) & (CONSOLE_INPUT_BUFFER_SIZE - 1U) };
        if (next != console_input_read.load(std::memory_order_acquire)) {
            console_input_buffer[write] = character;
            console_input_write.store(next, std::memory_order_release);
        }
        else {
            notify_reader = true;
        }
        // Wake a blocked raw reader as soon as data arrives. Waiting for a
        // newline or IDLE loses bytes on continuous newline-free input once
        // the ring fills, even when the reader could have drained it in time.
        notify_reader = true;
    }

    if ((COM1_UART->ISR & USART_ISR_IDLE) != 0U) {
        COM1_UART->ICR = USART_ICR_IDLECF;
        notify_reader = true;
    }

    COM1_UART->ICR = USART_ICR_PECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_ORECF;
    if (notify_reader) {
        static_cast<void>(tx_semaphore_ceiling_put(&console_input_available, 1U));
    }
}

extern "C" [[gnu::weak, gnu::noinline, noreturn]] void hal_panic_handler(const HalPanicInfo* info) noexcept
{
    __disable_irq();
    __DSB();
    write_panic_info(info);
    configure_red_led();

    while (true) {
        set_red_led(true);
        panic_delay();
        set_red_led(false);
        panic_delay();
    }
}
